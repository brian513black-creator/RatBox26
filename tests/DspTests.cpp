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

float sineRms(double rate, float hz, float amplitude, const ratbox::Settings& settings) {
    ratbox::VocalChain dsp;
    dsp.prepare(rate);
    std::array<float, block> samples{};
    double energy = 0;
    for (int b = 0; b < 96; ++b) {
        for (int i = 0; i < block; ++i)
            samples[i] = amplitude * std::sin(6.283185307 * hz * (b * block + i) / rate);
        dsp.process(samples.data(), nullptr, block, settings);
        if (b >= 80)
            for (const auto x : samples) energy += x * x;
    }
    return static_cast<float>(std::sqrt(energy / (16 * block)));
}

void testLowCutAndDynamics(double rate) {
    const auto dry = neutral();
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

void testBackgroundAndOrder(double rate) {
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
            dry[i] = wet[i] = .011f * (static_cast<float>(random >> 16) / 32768.f - 1.f);
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

    clean.prepare(rate);
    reference.prepare(rate);
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
    samples.fill(std::numeric_limits<float>::quiet_NaN());
    dsp.process(samples.data(), nullptr, block, s);
    require(std::all_of(samples.begin(), samples.end(), [](float x) { return std::isfinite(x); }),
            "Nonfinite input should not propagate");
}

} // namespace

int main() {
    for (const auto rate : {44100., 48000., 96000.}) {
        testLowCutAndDynamics(rate);
        testBackgroundAndOrder(rate);
        testReverb(rate);
        testBypassAndSafety(rate);
    }
    std::cout << "Vocal chain checks passed at 44.1, 48, and 96 kHz\n";
}
