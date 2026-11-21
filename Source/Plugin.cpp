#include <juce_audio_utils/juce_audio_utils.h>
#include "VocalChain.h"

namespace {

using APVTS = juce::AudioProcessorValueTreeState;
constexpr size_t knobCount = 11;
struct Control {
    const char* id;
    const char* label;
    const char* unit;
    float min, max, initial, step;
};
constexpr std::array<Control, knobCount> controls{{
    {"noise", "Background Noise", "%", 0, 100, 45, 1},
    {"deess", "De-ess", "%", 0, 100, 30, 1},
    {"character", "Character", "%", 0, 100, 32, 1},
    {"comp", "Compression", "%", 0, 100, 34, 1},
    {"space", "Reverb Mix", "%", 0, 100, 15, 1},
    {"decay", "Decay", " s", .35f, 5.f, 1.65f, .01f},
    {"duck", "Vocal Ducking", "%", 0, 100, 65, 1},
    {"output", "Output", " dB", -18, 12, 0, .1f},
    {"lowCut", "Low Cut", " Hz", 70, 350, 70, 1},
    {"focusHz", "Focus Frequency", " Hz", 150, 500, 280, 1},
    {"focusDb", "Focus Gain", " dB", -6, 6, 0, .1f}
}};

APVTS::ParameterLayout makeParameters() {
    APVTS::ParameterLayout layout;
    for (const auto& c : controls) {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{c.id, 1}, c.label,
            juce::NormalisableRange<float>{c.min, c.max, c.step}, c.initial,
            juce::AudioParameterFloatAttributes().withLabel(c.unit)));
    }
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"bypass", 1}, "Bypass", false));
    return layout;
}

class Editor;

class Processor final : public juce::AudioProcessor {
public:
    Processor() : AudioProcessor(BusesProperties()
                    .withInput("Input", juce::AudioChannelSet::stereo(), true)
                    .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
                  parameters(*this, nullptr, "RatBoxState", makeParameters()) {
        for (size_t i = 0; i < controls.size(); ++i)
            values[i] = parameters.getRawParameterValue(controls[i].id);
        bypass = parameters.getRawParameterValue("bypass");
    }

    const juce::String getName() const override { return "RatBox26"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    juce::AudioProcessorParameter* getBypassParameter() const override {
        return parameters.getParameter("bypass");
    }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    void prepareToPlay(double sampleRate, int) override {
        dsp.prepare(sampleRate);
        for (auto& meter : meters) meter.store(0, std::memory_order_relaxed);
    }
    void releaseResources() override {}
    void reset() override {
        dsp.reset();
        for (auto& meter : meters) meter.store(0, std::memory_order_relaxed);
    }
    bool isBusesLayoutSupported(const BusesLayout& layout) const override {
        const auto output = layout.getMainOutputChannelSet();
        const auto input = layout.getMainInputChannelSet();
        return (input == juce::AudioChannelSet::mono() &&
                (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo())) ||
               (input == juce::AudioChannelSet::stereo() && output == juce::AudioChannelSet::stereo());
    }

    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals guard;
        if (buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0) return;
        const auto value = [this](size_t i) {
            const auto raw = values[i]->load(std::memory_order_relaxed);
            const auto& control = controls[i];
            return std::isfinite(raw) ? std::clamp(raw, control.min, control.max) : control.initial;
        };
        const ratbox::Settings settings{ value(0) * .01f, value(1) * .01f, value(2) * .01f,
            value(3) * .01f, value(4) * .01f, value(5), value(6) * .01f, value(7),
            bypass->load(std::memory_order_relaxed) > .5f, value(8), value(9), value(10) };
        if (getTotalNumInputChannels() == 1 && getTotalNumOutputChannels() == 2)
            buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
        const auto result = dsp.process(buffer.getWritePointer(0),
            buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr,
            buffer.getNumSamples(), settings);
        const float readings[]{result.input, result.output, result.noise, result.comp};
        for (size_t i = 0; i < meters.size(); ++i) {
            auto previous = meters[i].load(std::memory_order_relaxed);
            while (previous < readings[i] &&
                   !meters[i].compare_exchange_weak(previous, readings[i], std::memory_order_relaxed)) {}
        }
    }
    void getStateInformation(juce::MemoryBlock& block) override {
        if (const auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, block);
    }
    void setStateInformation(const void* data, int size) override {
        if (const auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }

    APVTS parameters;
    std::array<std::atomic<float>, 4> meters{};

private:
    ratbox::VocalChain dsp;
    std::array<std::atomic<float>*, knobCount> values{};
    std::atomic<float>* bypass = nullptr;
};

const juce::Colour ink{0xffeef4f0}, muted{0xff9aaca6}, mint{0xff72f0c2};
const juce::Colour amber{0xffffbf83}, card{0xff181f20}, line{0xff354443};
const juce::Colour base{0xff111617};

juce::Font font(float height, bool bold = false) {
    return juce::Font{juce::FontOptions{height, bold ? juce::Font::bold : juce::Font::plain}};
}

class KnobLook final : public juce::LookAndFeel_V4 {
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos,
                          float first, float last, juce::Slider& slider) override {
        auto bounds = juce::Rectangle<float>{static_cast<float>(x), static_cast<float>(y),
            static_cast<float>(w), static_cast<float>(h)}.reduced(7.f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * .45f;
        const auto center = bounds.getCentre();
        const auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
        const float angle = first + pos * (last - first);
        const auto arc = [&](float from, float to, juce::Colour colour) {
            juce::Path path;
            path.addCentredArc(center.x, center.y, radius, radius, 0.f, from, to, true);
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType{3.f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded});
        };
        arc(first, last, line);
        if (pos > 0.f) arc(first, angle, accent);
        const float cap = radius - 8.f;
        juce::Rectangle<float> face{center.x - cap, center.y - cap, cap * 2.f, cap * 2.f};
        g.setGradientFill({juce::Colour{0xff334140}, face.getTopLeft(),
                           base, face.getBottomRight(), false});
        g.fillEllipse(face);
        g.setColour(line);
        g.drawEllipse(face, 1.f);
        juce::Path pointer;
        pointer.addRoundedRectangle(-1.55f, -cap + 6.f, 3.1f, 9.f, 1.5f);
        g.setColour(accent);
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(center.x, center.y));
    }
};

class Editor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit Editor(Processor& p) : AudioProcessorEditor(p), processor(p) {
        setOpaque(true);
        for (size_t i = 0; i < controls.size(); ++i) {
            const auto& c = controls[i];
            auto& knob = knobs[i];
            knob.setLookAndFeel(&look);
            knob.setName(c.label);
            knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 23);
            knob.setColour(juce::Slider::rotarySliderFillColourId, i >= 4 && i <= 6 ? amber : mint);
            knob.setColour(juce::Slider::textBoxTextColourId, ink);
            knob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            knob.setTextValueSuffix(c.unit);
            addAndMakeVisible(knob);
            bindings[i] = std::make_unique<APVTS::SliderAttachment>(p.parameters, c.id, knob);
            knob.setDoubleClickReturnValue(true, c.initial);
        }
        bypass.setButtonText("Bypass");
        bypass.setColour(juce::ToggleButton::textColourId, ink);
        bypass.setColour(juce::ToggleButton::tickColourId, mint);
        addAndMakeVisible(bypass);
        setResizable(true, true);
        setResizeLimits(760, 710, 1100, 940);
        setSize(840, 790);
        startTimerHz(30);
    }
    ~Editor() override {
        stopTimer();
        for (auto& knob : knobs) knob.setLookAndFeel(nullptr);
    }

    void resized() override {
        bypass.setBounds(getWidth() - 112, 34, 86, 32);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < columnCount(row); ++column) {
                const auto cell = controlCell(row, column);
                knobs[controlIndex(row, column)].setBounds(cell.withTrimmedTop(24).reduced(8, 2));
            }
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(base);
        g.setGradientFill({juce::Colour{0xff1d302b}, 0.f, 0.f, base, 0.f, 200.f, false});
        g.fillRect(getLocalBounds().withHeight(200));
        g.setColour(mint);
        g.fillRoundedRectangle(24.f, 32.f, 4.f, 57.f, 2.f);
        write(g, "RatBox26", {39, 26, 350, 46}, 36.f, ink, true);
        write(g, "VOCAL CHANNEL STRIP", {41, 72, 350, 18}, 12.f, mint, true);

        for (int row = 0; row < 3; ++row) {
            const auto panel = section(row);
            g.setColour(card);
            g.fillRoundedRectangle(panel.toFloat(), 14.f);
            g.setColour(line);
            g.drawRoundedRectangle(panel.toFloat(), 14.f, 1.f);
            constexpr const char* sectionNames[]{"CLEANUP", "TONAL FOCUS & DYNAMICS", "SPACE & OUTPUT"};
            write(g, sectionNames[row], panel.reduced(18, 0).removeFromTop(34),
                  12.f, row == 2 ? amber : mint, true);
            for (int col = 0; col < columnCount(row); ++col) {
                const auto i = controlIndex(row, col);
                const auto cell = controlCell(row, col);
                if (col > 0) {
                    g.setColour(line);
                    g.drawVerticalLine(cell.getX(), static_cast<float>(cell.getY() + 6),
                                       static_cast<float>(cell.getBottom() - 8));
                }
                write(g, controls[i].label, cell.withHeight(23), 14.f, ink, true,
                      juce::Justification::centred);
            }
        }

        auto metrics = getLocalBounds().reduced(24, 0).withY(getHeight() - 92).withHeight(65);
        g.setColour(card);
        g.fillRoundedRectangle(metrics.toFloat(), 12.f);
        metrics.reduce(14, 10);
        const int width = metrics.getWidth() / 4;
        const char* names[]{"INPUT", "OUTPUT", "NOISE REDUCTION", "COMP REDUCTION"};
        for (size_t i = 0; i < displayed.size(); ++i)
            drawMeter(g, metrics.removeFromLeft(width).reduced(7, 0), names[i], displayed[i], i >= 2);
    }

private:
    static int columnCount(int row) { return row == 0 ? 3 : 4; }
    static size_t controlIndex(int row, int column) {
        constexpr size_t groups[][4]{{8, 0, 1, 0}, {9, 10, 2, 3}, {4, 5, 6, 7}};
        return groups[row][column];
    }
    juce::Rectangle<int> section(int row) const {
        const int height = (getHeight() - 234) / 3;
        return {24, 109 + row * (height + 11), getWidth() - 48, height};
    }
    juce::Rectangle<int> controlCell(int row, int column) const {
        auto band = section(row).reduced(12, 0).withTrimmedTop(39).withTrimmedBottom(9);
        const int width = band.getWidth() / columnCount(row);
        return {band.getX() + column * width, band.getY(), width, band.getHeight()};
    }
    static void write(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                      float size, juce::Colour colour, bool bold, int align = juce::Justification::centredLeft) {
        g.setColour(colour);
        g.setFont(font(size, bold));
        g.drawText(text, area, align);
    }
    static void drawMeter(juce::Graphics& g, juce::Rectangle<int> area, const char* label,
                          float level, bool reduction) {
        auto heading = area.removeFromTop(22);
        const auto db = juce::Decibels::gainToDecibels(reduction ? 1.f - level : level, -60.f);
        const juce::String reading = !reduction && db <= -60.f ? "-inf" : juce::String(db, 1);
        write(g, label, heading, 10.f, muted, true);
        write(g, reading + " dB", heading, 11.f, ink, false, juce::Justification::centredRight);
        auto track = area.withTrimmedTop(6).withHeight(6).toFloat();
        g.setColour(line);
        g.fillRoundedRectangle(track, 3.f);
        const float fraction = reduction ? -db / 24.f : (db + 60.f) / 60.f;
        g.setColour(reduction ? amber : (level >= 1.f ? juce::Colour{0xffff857c} : mint));
        const float width = track.getWidth() * juce::jlimit(0.f, 1.f, fraction);
        if (width > 0.f) g.fillRoundedRectangle(track.withWidth(width), 3.f);
    }
    void timerCallback() override {
        for (size_t i = 0; i < displayed.size(); ++i)
            displayed[i] = std::max(displayed[i] * .86f,
                processor.meters[i].exchange(0.f, std::memory_order_relaxed));
        repaint(0, getHeight() - 96, getWidth(), 96);
    }

    Processor& processor;
    KnobLook look;
    std::array<juce::Slider, knobCount> knobs;
    std::array<std::unique_ptr<APVTS::SliderAttachment>, knobCount> bindings;
    juce::ToggleButton bypass;
    APVTS::ButtonAttachment bypassBinding{processor.parameters, "bypass", bypass};
    std::array<float, 4> displayed{};
};

juce::AudioProcessorEditor* Processor::createEditor() { return new Editor(*this); }

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Processor; }
