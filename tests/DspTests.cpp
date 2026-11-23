#include "VocalChain.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

constexpr int block = 256;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ratbox::Settings neutral() {
    ratbox::Settings s;
    s.noise = s.deess = s.character = s.comp = s.space = s.duck = 0;
    return s;
}

float sineRms(double rate, float hz, float amplitude, const ratbox::Settings& settings, int blocks = 96) {
    ratbox::VocalChain dsp;
    dsp.prepare(rate);
    std::array<float, block> samples{};
    double energy = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i)
            samples[i] = amplitude * std::sin(6.283185307 * hz * (b * block + i) / rate);
        dsp.process(samples.data(), nullptr, block, settings);
        if (b >= blocks - 16)
            for (const auto x : samples) energy += x * x;
    }
    return static_cast<float>(std::sqrt(energy / (16 * block)));
}

void testLowCutAndDynamics(double rate) {
    const auto dry = neutral();
    auto bypassed = dry;
    bypassed.bypass = true;
    for (const auto cutoff : {70.f, 350.f}) {
        auto filtered = dry;
        filtered.lowCut = cutoff;
        const auto ratio = sineRms(rate, cutoff, .2f, filtered) / sineRms(rate, cutoff, .2f, bypassed);
        require(std::abs(ratio - .7071f) < .025f, "Low cut should be about -3 dB at its selected frequency");
    }

    auto filtered = dry;
    filtered.lowCut = 350;
    require(sineRms(rate, 125, .2f, filtered) < .2f * sineRms(rate, 125, .2f, dry),
            "350 Hz low cut should attenuate a 125 Hz signal");
    require(sineRms(rate, 1200, .2f, filtered) > .97f * sineRms(rate, 1200, .2f, dry),
            "Low cut should preserve upper vocal frequencies");

    auto focus = dry;
    focus.focusHz = 330;
    focus.focusDb = -6;
    require(sineRms(rate, 330, .2f, focus) < .52f * sineRms(rate, 330, .2f, dry),
            "Focus EQ should cut at its selected center");
    require(sineRms(rate, 3000, .2f, focus) > .98f * sineRms(rate, 3000, .2f, dry),
            "Focus EQ should leave distant frequencies substantially unchanged");
    focus.focusDb = 6;
    require(sineRms(rate, 330, .2f, focus) > 1.95f * sineRms(rate, 330, .2f, dry),
            "Focus EQ should boost at its selected center");

    auto cleaned = dry;
    cleaned.noise = 1;
    require(sineRms(rate, 440, .002f, cleaned) < .25f * sineRms(rate, 440, .002f, dry),
            "Noise control should attenuate quiet background");
    require(sineRms(rate, 440, .12f, cleaned) > .99f * sineRms(rate, 440, .12f, dry),
            "Noise control should preserve clear vocal-level signal");
    const int sustainedBlocks = static_cast<int>(6 * rate / block);
    require(sineRms(rate, 440, .026f, cleaned, sustainedBlocks) >
            .99f * sineRms(rate, 440, .026f, dry, sustainedBlocks),
            "Noise control should keep a quiet tonal note above the learnable floor");

    cleaned = dry;
    cleaned.deess = 1;
    require(sineRms(rate, 8500, .2f, cleaned) < .7f * sineRms(rate, 8500, .2f, dry),
            "De-esser should reduce loud sibilant bands");
    cleaned = dry;
    cleaned.character = 1;
    require(sineRms(rate, 8000, .065f, cleaned) > 1.12f * sineRms(rate, 8000, .065f, dry),
            "Character should add high-frequency presence");
    cleaned = dry;
    cleaned.comp = 1;
    require(sineRms(rate, 440, .5f, cleaned) < .65f * sineRms(rate, 440, .5f, dry),
            "Compressor should reduce high levels");
}

void testLowCutSwitch(double rate) {
    ratbox::VocalChain switched, reference;
    switched.prepare(rate);
    reference.prepare(rate);
    const auto enabled = neutral();
    auto settings = enabled;
    std::array<float, block> input{}, output{}, filtered{};
    const int phaseLength = static_cast<int>(.04 * rate);
    for (int phase = 0; phase < 3; ++phase) {
        settings.lowCutEnabled = phase == 1;
        for (int start = 0; start < phaseLength; start += block) {
            const int frames = std::min(block, phaseLength - start);
            for (int i = 0; i < frames; ++i) {
                const int position = phase * phaseLength + start + i;
                input[i] = output[i] = filtered[i] = .25f * std::cos(6.283185307 * 70 * position / rate);
            }
            reference.process(filtered.data(), nullptr, frames, enabled);
            switched.process(output.data(), nullptr, frames, settings);
            for (int i = 0; i < frames; ++i) {
                const float progress = std::min(1.f, static_cast<float>((start + i + 1) / (.01 * rate)));
                const float mix = phase == 0 ? 0.f : phase == 1 ? progress : 1.f - progress;
                const float expected = input[i] * (1.f - mix) + filtered[i] * mix;
                require(std::abs(output[i] - expected) < .00001f,
                        "Low Cut should crossfade against the warm filter output");
                if (phase == 0 || (phase == 2 && start + i > .02 * rate))
                    require(output[i] == input[i], "Low Cut off should leave a neutral chain exactly dry");
            }
        }
    }
}

void testFocusFrequencyAndOutput(double rate) {
    const auto dry = neutral();
    auto boost = dry;
    boost.focusDb = 6.f;
    auto cut = boost;
    cut.focusDb = -6.f;
    for (const auto hz : {184.f, 427.f}) {
        const float flatRms = sineRms(rate, hz, .2f, dry);
        const float corrected = sineRms(rate, hz, .2f, boost) * sineRms(rate, hz, .2f, cut);
        require(std::abs(corrected / (flatRms * flatRms) - 1.f) < .02f,
                "Matching focus boosts and cuts should cancel on both shoulders of the bell");
    }

    auto lowFocus = dry;
    lowFocus.focusHz = 150.f;
    lowFocus.focusDb = 6.f;
    auto highFocus = lowFocus;
    highFocus.focusHz = 500.f;
    require(sineRms(rate, 150, .2f, lowFocus) > 1.5f * sineRms(rate, 150, .2f, highFocus),
            "Low focus frequency should target the low end of the EQ band");
    require(sineRms(rate, 500, .2f, highFocus) > 1.5f * sineRms(rate, 500, .2f, lowFocus),
            "High focus frequency should target the high end of the EQ band");

    auto neutralControls = dry;
    neutralControls.focusHz = 500.f;
    neutralControls.decay = 5.f;
    neutralControls.duck = 1.f;
    const float reference = sineRms(rate, 500, .2f, dry);
    require(std::abs(sineRms(rate, 500, .2f, neutralControls) - reference) < 1.e-6f,
            "Focus frequency, decay and ducking should be inaudible with their levels at zero");

    for (const auto db : {-18.f, 12.f}) {
        auto adjusted = dry;
        adjusted.output = db;
        const float ratio = sineRms(rate, 500, .2f, adjusted) / reference;
        require(std::abs(ratio - std::pow(10.f, db / 20.f)) < .001f,
                "Output trim should apply the selected decibel gain");
    }
}

void testInputTrim(double rate) {
    auto settings = neutral();
    settings.lowCutEnabled = false;
    ratbox::VocalChain chain;
    chain.prepare(rate);
    constexpr float leftInput = .0625f, rightInput = -.125f;
    std::array<float, block> left{}, right{};
    for (const auto db : {-18.f, 18.f}) {
        chain.reset();
        settings.inputTrimDb = db;
        settings.bypass = false;
        left.fill(leftInput);
        right.fill(rightInput);
        const auto active = chain.process(left.data(), right.data(), block, settings);
        const auto gain = std::pow(10.f, db / 20.f);
        require(std::abs(left.front() - leftInput * gain) < 1.e-6f &&
                std::abs(right.front() - rightInput * gain) < 1.e-6f &&
                std::abs(active.input - std::abs(rightInput * gain)) < 1.e-6f,
                "Restored input trim should apply the selected gain to both channels and the input meter");

        chain.reset();
        settings.bypass = true;
        left.fill(leftInput);
        right.fill(rightInput);
        const auto bypassed = chain.process(left.data(), right.data(), block, settings);
        require(left.front() == leftInput && right.front() == rightInput && bypassed.input == std::abs(rightInput),
                "Global bypass should ignore input trim in its dry signal and input meter");
    }

    chain.reset();
    settings.bypass = false;
    settings.inputTrimDb = 0.f;
    left.fill(leftInput);
    chain.process(left.data(), nullptr, block, settings);
    settings.inputTrimDb = 18.f;
    left.fill(leftInput);
    chain.process(left.data(), nullptr, block, settings);
    require(left.front() > leftInput && left.front() < 1.01f * leftInput,
            "Changing input trim should ramp instead of jumping on the first sample");

    settings.comp = 1.f;
    for (const auto db : {0.f, 18.f}) {
        chain.reset();
        settings.inputTrimDb = db;
        ratbox::Meters meters;
        const int blocks = static_cast<int>(.25 * rate / block);
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < block; ++i)
                left[i] = leftInput * std::sin(6.283185307 * 440 * (b * block + i) / rate);
            meters = chain.process(left.data(), nullptr, block, settings);
        }
        require(db == 0.f ? meters.comp == 0.f : meters.comp > .4f,
                "Input trim should drive the compressor from upstream");
    }
}

void testStereoDeess(double rate) {
    ratbox::VocalChain tested, reference;
    tested.prepare(rate);
    reference.prepare(rate);
    const auto dry = neutral();
    auto deessed = dry;
    deessed.deess = 1.f;
    std::array<float, block> left{}, right{}, dryLeft{}, dryRight{};
    double energy = 0., dryEnergy = 0.;
    for (int b = 0; b < 96; ++b) {
        for (int i = 0; i < block; ++i) {
            const double phase = 6.283185307 * (b * block + i) / rate;
            left[i] = dryLeft[i] = .65f * std::sin(220. * phase);
            right[i] = dryRight[i] = .18f * std::sin(8500. * phase);
        }
        tested.process(left.data(), right.data(), block, deessed);
        reference.process(dryLeft.data(), dryRight.data(), block, dry);
        if (b >= 80)
            for (int i = 0; i < block; ++i) {
                energy += right[i] * right[i];
                dryEnergy += dryRight[i] * dryRight[i];
            }
    }
    require(energy < .49 * dryEnergy, "A loud low tone in one channel should not mask sibilance in the other");
}

void testBackgroundNoise(double rate, float peak) {
    ratbox::VocalChain clean, reference;
    clean.prepare(rate);
    reference.prepare(rate);
    auto enabled = neutral();
    enabled.noise = 1;
    const auto disabled = neutral();
    std::array<float, block> wet{}, dry{};
    std::uint32_t random = 12345;
    double wetEnergy = 0, dryEnergy = 0;
    const int blocks = static_cast<int>(6 * rate / block);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            random = 1664525u * random + 1013904223u;
            dry[i] = wet[i] = peak * (static_cast<float>(random >> 16) / 32768.f - 1.f);
        }
        clean.process(wet.data(), nullptr, block, enabled);
        reference.process(dry.data(), nullptr, block, disabled);
        if (b >= blocks - 20)
            for (int i = 0; i < block; ++i) {
                wetEnergy += wet[i] * wet[i];
                dryEnergy += dry[i] * dry[i];
            }
    }
    require(wetEnergy < .08 * dryEnergy, "Noise floor tracking should attenuate steady broadband noise");
}

void testStereoNoiseLink(double rate) {
    ratbox::VocalChain tested, reference;
    tested.prepare(rate);
    reference.prepare(rate);
    const auto dry = neutral();
    auto reduced = dry;
    reduced.noise = 1.f;
    std::array<float, block> left{}, right{}, dryLeft{}, dryRight{};
    std::uint32_t random = 784217;
    double energy = 0., dryEnergy = 0., reducedHiss = 0., dryHiss = 0.;
    const int blocks = static_cast<int>(7 * rate / block);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            const int sample = b * block + i;
            const double phase = 6.283185307 * 440. * sample / rate;
            left[i] = dryLeft[i] = sample < 3 * rate ? 0.f : .026f * std::sin(phase);
            random = 1664525u * random + 1013904223u;
            right[i] = dryRight[i] = .034f * (static_cast<float>(random >> 16) / 32768.f - 1.f);
        }
        tested.process(left.data(), right.data(), block, reduced);
        reference.process(dryLeft.data(), dryRight.data(), block, dry);
        if (b * block >= 2.8 * rate && (b + 1) * block <= 3 * rate)
            for (int i = 0; i < block; ++i) {
                reducedHiss += right[i] * right[i];
                dryHiss += dryRight[i] * dryRight[i];
            }
        if (b >= blocks - 20)
            for (int i = 0; i < block; ++i) {
                energy += left[i] * left[i];
                dryEnergy += dryLeft[i] * dryLeft[i];
            }
    }
    require(reducedHiss < .08 * dryHiss, "Noise reduction should still learn hiss during a stereo pause");
    require(energy > .98 * dryEnergy, "Hiss on one channel should not suppress a clean note on the other");
}

void testNoiseLearningAfterHum(double rate) {
    ratbox::VocalChain tested, reference;
    tested.prepare(rate);
    reference.prepare(rate);
    const auto dry = neutral();
    auto reduced = dry;
    reduced.noise = 1.f;
    std::array<float, block> wet{}, flat{};
    double humEnergy = 0., dryHumEnergy = 0.;
    double earlyEnergy = 0., dryEarlyEnergy = 0.;
    double lateEnergy = 0., dryLateEnergy = 0.;
    const int sectionBlocks = static_cast<int>(6 * rate / block);
    const int windowBlocks = static_cast<int>(.15 * rate / block);
    const int earlyStart = sectionBlocks + windowBlocks;
    const int lateStart = 2 * sectionBlocks - windowBlocks;
    for (int b = 0; b < 2 * sectionBlocks; ++b) {
        const bool hum = b < sectionBlocks;
        const auto peak = hum ? .012f : .026f;
        const auto frequency = hum ? 250. : 440.;
        for (int i = 0; i < block; ++i)
            wet[i] = flat[i] = peak * std::sin(6.283185307 * frequency * (b * block + i) / rate);
        tested.process(wet.data(), nullptr, block, reduced);
        reference.process(flat.data(), nullptr, block, dry);
        for (int i = 0; i < block; ++i) {
            if (hum && b >= sectionBlocks - windowBlocks) {
                humEnergy += wet[i] * wet[i];
                dryHumEnergy += flat[i] * flat[i];
            } else if (b >= earlyStart && b < earlyStart + windowBlocks) {
                earlyEnergy += wet[i] * wet[i];
                dryEarlyEnergy += flat[i] * flat[i];
            } else if (b >= lateStart) {
                lateEnergy += wet[i] * wet[i];
                dryLateEnergy += flat[i] * flat[i];
            }
        }
    }
    require(humEnergy < .08 * dryHumEnergy, "Noise reduction should still learn low-frequency hum");
    require(lateEnergy > .10 * dryLateEnergy &&
            lateEnergy / dryLateEnergy > .90 * earlyEnergy / dryEarlyEnergy,
            "A stronger sustained note should not fade as the noise floor adapts");
}

void testSignalOrder(double rate) {
    ratbox::VocalChain clean, reference;
    clean.prepare(rate);
    reference.prepare(rate);
    std::array<float, block> wet{}, dry{};
    auto flatComp = neutral();
    flatComp.comp = 1;
    auto shapedComp = flatComp;
    shapedComp.focusDb = 6;
    ratbox::Meters shaped{}, flat{};
    for (int b = 0; b < 80; ++b) {
        for (int i = 0; i < block; ++i)
            dry[i] = wet[i] = .13f * std::sin(6.283185307 * 280 * (b * block + i) / rate);
        shaped = clean.process(wet.data(), nullptr, block, shapedComp);
        flat = reference.process(dry.data(), nullptr, block, flatComp);
    }
    require(shaped.comp > flat.comp + .1f, "Compression should react to tonal focus upstream");
}

void testNoiseOnset(double rate) {
    ratbox::VocalChain clean, reference;
    clean.prepare(rate);
    reference.prepare(rate);
    const auto dry = neutral();
    auto reduced = dry;
    reduced.noise = 1.f;
    std::array<float, block> wet{}, flat{};
    const int quietBlocks = static_cast<int>(rate / block);
    for (int b = 0; b < quietBlocks; ++b) {
        clean.process(wet.data(), nullptr, block, reduced);
        reference.process(flat.data(), nullptr, block, dry);
    }

    const int onset = static_cast<int>(rate * .002);
    for (int i = 0; i < onset; ++i)
        wet[i] = flat[i] = .25f * std::sin(6.283185307 * 1000 * i / rate);
    clean.process(wet.data(), nullptr, onset, reduced);
    reference.process(flat.data(), nullptr, onset, dry);
    float reducedEnergy = 0.f, dryEnergy = 0.f;
    for (int i = 0; i < onset; ++i) {
        reducedEnergy += wet[i] * wet[i];
        dryEnergy += flat[i] * flat[i];
    }
    require(reducedEnergy > .70f * dryEnergy,
            "Noise reduction should preserve short, strong vocal onsets after a pause");
}

void testReverb(double rate) {
    ratbox::VocalChain room, ducked, dry;
    room.prepare(rate);
    ducked.prepare(rate);
    dry.prepare(rate);
    auto wet = neutral();
    wet.space = 1;
    auto smart = wet;
    smart.duck = 1;
    const auto flat = neutral();
    std::array<float, block> left{}, right{}, reference{}, duckL{}, duckR{};
    float tail = 0, stereo = 0, unduckedDifference = 0, duckedDifference = 0;
    for (int b = 0; b < 185; ++b) {
        for (int i = 0; i < block; ++i) {
            const float x = b < 65
                ? .4f * std::sin(6.283185307 * 260 * (b * block + i) / rate) : 0.f;
            left[i] = right[i] = reference[i] = duckL[i] = duckR[i] = x;
        }
        room.process(left.data(), right.data(), block, wet);
        ducked.process(duckL.data(), duckR.data(), block, smart);
        dry.process(reference.data(), nullptr, block, flat);
        for (int i = 0; i < block; ++i) {
            if (b >= 48 && b < 65) {
                unduckedDifference += std::abs(left[i] - reference[i]);
                duckedDifference += std::abs(duckL[i] - reference[i]);
            }
            if (b >= 75 && b < 115) {
                tail += std::abs(left[i] - reference[i]);
                stereo += std::abs(left[i] - right[i]);
            }
        }
    }
    require(tail > 1.f && stereo > 1.f, "Reverb should produce a stereo tail after vocal stops");
    require(duckedDifference < .4f * unduckedDifference, "Smart ducking should reduce reverb under the vocal");
}

void testReverbLevelAndDecay(double rate) {
    const auto dry = neutral();
    auto longRoom = dry;
    longRoom.space = 1.f;
    longRoom.decay = 5.f;
    auto quietRoom = longRoom;
    quietRoom.space = .4f;
    auto shortRoom = longRoom;
    shortRoom.decay = .35f;
    const std::array settings{dry, longRoom, quietRoom, shortRoom};
    std::array<ratbox::VocalChain, settings.size()> chains;
    std::array<std::array<float, block>, settings.size()> audio{};
    for (auto& chain : chains) chain.prepare(rate);

    double longEnergy = 0., shortEnergy = 0., levelError = 0.;
    const int blocks = static_cast<int>(.8 * rate / block);
    const int tailStart = static_cast<int>(.4 * rate / block);
    for (int b = 0; b < blocks; ++b) {
        for (size_t i = 0; i < chains.size(); ++i) {
            auto& signal = audio[i];
            signal.fill(0.f);
            if (b == 0) signal[0] = .6f;
            chains[i].process(signal.data(), nullptr, block, settings[i]);
        }
        if (b < tailStart) continue;
        for (int i = 0; i < block; ++i) {
            const double longTail = audio[1][i] - audio[0][i];
            const double quietTail = audio[2][i] - audio[0][i];
            const double shortTail = audio[3][i] - audio[0][i];
            const double residual = quietTail - .4 * longTail;
            longEnergy += longTail * longTail;
            shortEnergy += shortTail * shortTail;
            levelError += residual * residual;
        }
    }
    require(longEnergy > 1.e-5 && longEnergy > 20. * shortEnergy,
            "Long decay should preserve substantially more late reverb energy than short decay");
    require(levelError < 1.e-6 * longEnergy, "Reverb level should scale the wet signal");
}

void testCharacterCurve() {
    auto settings = neutral();
    settings.lowCutEnabled = false;
    settings.character = 1.f;
    ratbox::VocalChain dsp;
    dsp.prepare(48000.);
    constexpr float drive = 1.65f;
    std::array<float, block> audio{};
    for (const float input : {.12f, .60f, .61f, .85f, -.60f}) {
        dsp.reset();
        for (int b = 0; b < 24; ++b) {
            audio.fill(input);
            dsp.process(audio.data(), nullptr, block, settings);
        }
        const float expected = input + .4f * (std::tanh(input * drive) / drive - input);
        require(std::abs(audio.back() - expected) < .000002f,
                "Character should match its saturation curve on both sides of the fast-path cutoff");
    }
}

void testCharacterFoldback() {
    constexpr int rate = 44100;
    constexpr int frequency = 13000;
    constexpr int foldedThird = rate - 3 * frequency;
    ratbox::VocalChain dsp;
    dsp.prepare(rate);
    auto settings = neutral();
    settings.character = 1.f;
    std::array<float, block> audio{};
    double fundamentalSin = 0., fundamentalCos = 0., foldSin = 0., foldCos = 0.;
    for (int start = 0; start < rate; start += block) {
        const int frames = std::min(block, rate - start);
        for (int i = 0; i < frames; ++i)
            audio[i] = .8f * std::sin(6.283185307 * frequency * (start + i) / rate);
        dsp.process(audio.data(), nullptr, frames, settings);
        for (int i = std::max(0, rate / 2 - start); i < frames; ++i) {
            const double phase = 6.283185307 * (start + i) / rate;
            fundamentalSin += audio[i] * std::sin(frequency * phase);
            fundamentalCos += audio[i] * std::cos(frequency * phase);
            foldSin += audio[i] * std::sin(foldedThird * phase);
            foldCos += audio[i] * std::cos(foldedThird * phase);
        }
    }
    require(std::hypot(foldSin, foldCos) < .004 * std::hypot(fundamentalSin, fundamentalCos),
            "Character should keep high-frequency foldback below -48 dBc on a hot 13 kHz input");
}

void testBypassAndSafety(double rate) {
    ratbox::VocalChain dsp;
    dsp.prepare(rate);
    ratbox::Settings s;
    s.bypass = true;
    std::array<float, block> samples{};
    samples.fill(.22f);
    dsp.process(samples.data(), nullptr, block, s);
    require(samples.front() == .22f && samples.back() == .22f,
            "Restored bypass should pass dry input from the first sample");
    s.bypass = false;
    const int fadeBlocks = static_cast<int>(rate * .012 / block) + 1;
    for (int b = 0; b < fadeBlocks; ++b) {
        samples.fill(.22f);
        dsp.process(samples.data(), nullptr, block, s);
    }
    s.bypass = true;
    for (int b = 0; b < fadeBlocks; ++b) {
        samples.fill(.22f);
        dsp.process(samples.data(), nullptr, block, s);
    }
    for (int i = 0; i < block; ++i)
        samples[i] = i % 2 == 0 ? 70.f : -70.f;
    dsp.process(samples.data(), nullptr, block, s);
    for (int i = 0; i < block; ++i)
        require(samples[i] == (i % 2 == 0 ? 70.f : -70.f),
                "Bypass should preserve hot float input exactly after the transition");

    samples.fill(std::numeric_limits<float>::quiet_NaN());
    dsp.process(samples.data(), nullptr, block, s);
    require(std::all_of(samples.begin(), samples.end(), [](float x) { return std::isfinite(x); }),
            "Nonfinite input should not propagate");
}

} // namespace

int main() {
    testCharacterCurve();
    testCharacterFoldback();
    for (const auto rate : {44100., 48000., 96000.}) {
        testLowCutAndDynamics(rate);
        testLowCutSwitch(rate);
        testFocusFrequencyAndOutput(rate);
        testInputTrim(rate);
        testStereoDeess(rate);
        testBackgroundNoise(rate, .011f);
        testBackgroundNoise(rate, .026f);
        testStereoNoiseLink(rate);
        testNoiseLearningAfterHum(rate);
        testSignalOrder(rate);
        testNoiseOnset(rate);
        testReverb(rate);
        testReverbLevelAndDecay(rate);
        testBypassAndSafety(rate);
    }
    std::cout << "Vocal chain checks passed at 44.1, 48, and 96 kHz\n";
}
