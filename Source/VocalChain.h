#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace ratbox {

struct Settings {
    float noise = .45f, deess = .30f, character = .32f, comp = .34f;
    float space = .15f, decay = 1.65f, duck = .65f, output = 0.f;
    bool bypass = false;
    float lowCut = 70.f, focusHz = 280.f, focusDb = 0.f;
    bool lowCutEnabled = true;
    float inputTrimDb = 0.f;
};

struct Meters {
    float input = 0, output = 0, noise = 0, comp = 0;
};

class VocalChain {
public:
    void prepare(double sampleRate) {
        *this = VocalChain{};
        rate = static_cast<float>(sampleRate);
        lowCutG = cutoffCoefficient(70.f);
        focusG = cutoffCoefficient(280.f);
        presence = filter(2800);
        air = filter(8200);
        sibilance = cutoffCoefficient(5700);
        sibilance /= 1.f + sibilance;
        colourPole = cutoffCoefficient(5500);
        colourPole /= 1.f + colourPole;
        wetLow = filter(220);
        wetHigh = filter(6100);
        roomDamping = filter(2830);
        fast = time(.003f);
        switchStep = 1.f / (rate * .01f);
        noiseOpen = time(.00025f);
        release = time(.085f);
        compAttack = time(.012f);
        compRelease = time(.105f);
        smooth = time(.024f);
        floorRise = time(1.8f);
        floorFall = time(.19f);
        duckRelease = time(.32f);
        for (size_t channel = 0; channel < 2; ++channel) {
            auto& room = rooms[channel];
            room.pre.allocate(rate, .025f + .004f * channel);
            const float combTimes[] = {.0297f, .0371f, .0411f, .0437f};
            for (size_t i = 0; i < room.combs.size(); ++i)
                room.combs[i].allocate(rate, combTimes[i] + .0019f * channel);
            room.diffuse[0].allocate(rate, .005f + .0006f * channel);
            room.diffuse[1].allocate(rate, .0017f + .0004f * channel);
        }
    }

    void reset() {
        channels = {};
        for (auto& room : rooms) {
            room.pre.clear();
            for (auto& comb : room.combs) comb.clear();
            for (auto& delay : room.diffuse) delay.clear();
            room.low = room.high = 0.f;
        }
        compPower = 0.f;
        noiseGain = compGain = duckGain = 1.f;
        deessGain = 0.f;
        initialized = false;
    }

    Meters process(float* left, float* right, int frames, const Settings& target) {
        Meters meters;
        if (std::islessgreater(target.decay, cachedDecay)) {
            const float decaySamples = rate * target.decay;
            for (auto& room : rooms)
                for (auto& comb : room.combs)
                    comb.feedbackTarget = std::pow(.001f, static_cast<float>(comb.samples.size()) / decaySamples);
            cachedDecay = target.decay;
        }

        const auto frequencyToCoefficient = [this](float hz) { return cutoffCoefficient(hz); };
        const auto decibelsToGain = [](float db) { return std::pow(10.f, db / 20.f); };
        const float cutoffTarget = cutoffCache.get(target.lowCut, frequencyToCoefficient);
        const float focusTarget = focusFrequencyCache.get(target.focusHz, frequencyToCoefficient);
        const float focusGainTarget = focusGainCache.get(target.focusDb, decibelsToGain) - 1.f;
        const float inputGainTarget = inputGainCache.get(target.inputTrimDb, decibelsToGain);
        const float outTarget = outputGainCache.get(target.output, decibelsToGain);
        const float activeTarget = target.bypass ? 0.f : 1.f;
        const float lowCutMixTarget = target.lowCutEnabled ? 1.f : 0.f;
        const float compThreshold = .25f - .175f * target.comp;
        const float compExponent = -(.64f * target.comp);
        if (!initialized) {
            lowCutG = cutoffTarget;
            lowCutMix = lowCutMixTarget;
            focusG = focusTarget;
            focusGain = focusGainTarget;
            tone = target.character;
            wet = target.space;
            inputGain = inputGainTarget;
            outGain = outTarget;
            active = activeTarget;
            for (auto& room : rooms)
                for (auto& comb : room.combs)
                    comb.feedback = comb.feedbackTarget;
            initialized = true;
        }
        constexpr float damping = 1.41421356f;
        float lowCutA1 = filterNormalization(lowCutG, damping);
        // Match the bell's boost and cut curves by exchanging the pole and zero damping.
        float focusDamping = focusDampingFor(focusGain);
        float focusA1 = filterNormalization(focusG, focusDamping);
        bool changingCutoff = std::islessgreater(lowCutG, cutoffTarget);
        bool changingFocus = std::islessgreater(focusG, focusTarget) ||
                             std::islessgreater(focusGain, focusGainTarget);
        for (int frame = 0; frame < frames; ++frame) {
            float dry[2] = {safe(left[frame]), right ? safe(right[frame]) : safe(left[frame])};
            float clean[2], high[2];
            follow(inputGain, inputGainTarget, smooth);
            if (changingCutoff) {
                changingCutoff = smoothCoefficient(lowCutG, cutoffTarget);
                if (changingCutoff)
                    lowCutA1 = filterNormalization(lowCutG, damping);
            }
            lowCutMix += std::clamp(lowCutMixTarget - lowCutMix, -switchStep, switchStep);
            if (changingFocus) {
                const bool frequencyChanged = smoothCoefficient(focusG, focusTarget);
                const bool gainChanged = smoothCoefficient(focusGain, focusGainTarget);
                changingFocus = frequencyChanged || gainChanged;
                if (changingFocus) {
                    focusDamping = focusDampingFor(focusGain);
                    focusA1 = filterNormalization(focusG, focusDamping);
                }
            }
            float levels[2], highLevels[2];
            float input = 0.f;
            float speech = 0.f;
            for (size_t channel = 0; channel < 2; ++channel) {
                auto& state = channels[channel];
                const float source = std::clamp(dry[channel] * inputGain, -64.f, 64.f);
                input = std::max(input, std::abs(source));
                const float v1 = lowCutA1 * (state.integrator1 + lowCutG * (source - state.integrator2));
                const float v2 = state.integrator2 + lowCutG * v1;
                state.integrator1 = 2.f * v1 - state.integrator1;
                state.integrator2 = 2.f * v2 - state.integrator2;
                const float filtered = source - damping * v1 - v2;
                clean[channel] = source * (1.f - lowCutMix) + filtered * lowCutMix;
                const float sibV = (clean[channel] - state.sib) * sibilance;
                const float sibLow = state.sib + sibV;
                state.sib = sibLow + sibV;
                high[channel] = clean[channel] - sibLow;
                envelope(state.levelPower, clean[channel] * clean[channel], fast, release);
                envelope(state.highPower, high[channel] * high[channel], fast, release);
                levels[channel] = std::sqrt(state.levelPower);
                highLevels[channel] = std::sqrt(state.highPower);

                // Learn each side separately so noise on one side cannot hide a voice on the other.
                const float level = levels[channel];
                const float candidate = std::clamp(level, .0002f, .035f);
                auto& floor = state.noiseFloor;
                // Only expand the learning range for hiss so a learned floor cannot absorb a quiet note.
                const float learningThreshold = highLevels[channel] > .55f * level
                    ? std::max(.030f, floor * 2.4f) : .016f;
                if (candidate < floor || level < learningThreshold)
                    follow(floor, candidate, candidate < floor ? floorFall : floorRise);
                const float snr = level / std::max(floor, .0015f);
                speech = std::max(speech, std::clamp((snr - 1.25f) / 2.25f, 0.f, 1.f));
            }

            // Either side can open the linked gain for both channels.
            const float noiseTarget = 1.f - .95f * target.noise * (1.f - speech * speech * (3.f - 2.f * speech));
            follow(noiseGain, noiseTarget, noiseTarget > noiseGain ? noiseOpen : release);

            // Detect on each side and link the gain to the strongest sibilance.
            float sibilant = 0.f;
            for (size_t channel = 0; channel < 2; ++channel) {
                const float ratio = highLevels[channel] / std::max(levels[channel], .008f);
                const float strength = std::clamp((ratio - .28f) * 2.6f, 0.f, 1.f) *
                    std::clamp((highLevels[channel] * noiseGain - .012f) * 65.f, 0.f, 1.f);
                sibilant = std::max(sibilant, strength);
            }
            const float sReduction = target.deess * .78f * sibilant;
            follow(deessGain, sReduction, sReduction > deessGain ? fast : release);

            follow(tone, target.character, smooth);
            follow(wet, target.space, smooth);
            follow(outGain, outTarget, smooth);
            active += std::clamp(activeTarget - active, -switchStep, switchStep);
            const float drive = 1.f + .65f * tone;

            for (size_t channel = 0; channel < 2; ++channel) {
                auto& state = channels[channel];
                float x = (clean[channel] - high[channel] * deessGain) * noiseGain;
                const float focusBand = focusA1 * (state.focus1 + focusG * (x - state.focus2));
                const float focusLow = state.focus2 + focusG * focusBand;
                state.focus1 = 2.f * focusBand - state.focus1;
                state.focus2 = 2.f * focusLow - state.focus2;
                x += focusBand * focusDamping * focusGain;
                const float upperMid = x - follow(state.presence, x, presence);
                const float top = x - follow(state.air, x, air);
                x += tone * (.23f * upperMid + .33f * top);
                // Keep the air shelf full-band while limiting what drives the nonlinear stage.
                const float bodyV = (x - state.colourLow) * colourPole;
                const float body = state.colourLow + bodyV;
                state.colourLow = body + bodyV;
                clean[channel] = tone > 0.f ? x + .4f * tone * saturationResidual(body, drive) : x;
            }

            const float shaped = std::max(std::abs(clean[0]), std::abs(clean[1]));
            envelope(compPower, shaped * shaped, fast, release);
            const float compLevel = std::sqrt(compPower);
            const float compTarget = compExponent < 0.f && compLevel > compThreshold
                ? std::pow(compLevel / compThreshold, compExponent) : 1.f;
            follow(compGain, compTarget, compTarget < compGain ? compAttack : compRelease);
            for (auto& sample : clean) sample *= compGain;

            const float duckTarget = 1.f - .91f * target.duck *
                std::clamp(compLevel * compGain * 12.f, 0.f, 1.f);
            follow(duckGain, duckTarget, duckTarget < duckGain ? compAttack : duckRelease);
            const float sends[]{.75f * clean[0] + .25f * clean[1], .75f * clean[1] + .25f * clean[0]};
            for (size_t channel = 0; channel < 2; ++channel) {
                auto& room = rooms[channel];
                const float predelayed = room.pre.exchange(sends[channel]);
                float reflected = 0;
                for (auto& comb : room.combs) {
                    const float previous = comb.read();
                    follow(comb.damped, previous, roomDamping);
                    follow(comb.feedback, comb.feedbackTarget, smooth);
                    comb.exchange(predelayed + comb.damped * comb.feedback);
                    reflected += previous * .25f;
                }
                for (auto& delay : room.diffuse) {
                    const float delayed = delay.read();
                    const float diffused = delayed - .55f * reflected;
                    delay.exchange(reflected + .55f * diffused);
                    reflected = diffused;
                }
                reflected -= follow(room.low, reflected, wetLow);
                follow(room.high, reflected, wetHigh);
                clean[channel] = (clean[channel] + room.high * wet * duckGain) * outGain;
            }

            const float stereoOrMono = right ? clean[0] : .5f * (clean[0] + clean[1]);
            left[frame] = dry[0] * (1.f - active) + stereoOrMono * active;
            if (right)
                right[frame] = dry[1] * (1.f - active) + clean[1] * active;
            if (active <= 0.f)
                input = std::max(std::abs(dry[0]), std::abs(dry[1]));
            meters.input = std::max(meters.input, input);
            meters.output = std::max(meters.output, std::max(std::abs(left[frame]),
                right ? std::abs(right[frame]) : 0.f));
            meters.noise = std::max(meters.noise, (1.f - noiseGain) * active);
            meters.comp = std::max(meters.comp, (1.f - compGain) * active);
        }
        return meters;
    }

private:
    struct CachedCoefficient {
        template <typename Convert>
        float get(float setting, Convert convert) {
            if (std::islessgreater(setting, previous)) {
                value = convert(setting);
                previous = setting;
            }
            return value;
        }

        float previous = std::numeric_limits<float>::infinity(), value = 0.f;
    };

    struct Delay {
        std::vector<float> samples;
        size_t cursor = 0;
        float damped = 0, feedback = 0, feedbackTarget = 0;
        void clear() {
            std::fill(samples.begin(), samples.end(), 0.f);
            cursor = 0;
            damped = feedback = 0.f;
        }
        void allocate(float rate, float seconds) {
            samples.assign(std::max<size_t>(1, static_cast<size_t>(rate * seconds)), 0.f);
        }
        float read() const { return samples[cursor]; }
        float exchange(float input) {
            const float previous = read();
            samples[cursor] = input;
            if (++cursor == samples.size()) cursor = 0;
            return previous;
        }
    };
    struct Room {
        Delay pre;
        std::array<Delay, 4> combs;
        std::array<Delay, 2> diffuse;
        float low = 0, high = 0;
    };
    struct Channel {
        float focus1 = 0, focus2 = 0;
        float integrator1 = 0, integrator2 = 0, sib = 0, presence = 0, air = 0;
        float colourLow = 0, levelPower = 0, highPower = 0;
        float noiseFloor = .0015f;
    };

    static float saturationResidual(float body, float drive) {
        const float driven = body * drive;
        const float square = driven * driven;
        if (square > 1.f)
            return std::tanh(driven) / drive - body;

        // The [5/4] Padé curve for tanh, with the dry term subtracted analytically.
        return -body * square * (315.f + 14.f * square) /
            (945.f + square * (420.f + 15.f * square));
    }

    static float safe(float x) { return std::isfinite(x) ? x : 0.f; }
    static float follow(float& state, float value, float coefficient) {
        state += coefficient * (value - state);
        return state;
    }
    static void envelope(float& state, float value, float attack, float release) {
        follow(state, value, value > state ? attack : release);
    }
    bool smoothCoefficient(float& state, float target) const {
        const float previous = state;
        follow(state, target, smooth);
        return std::islessgreater(state, previous);
    }
    static float filterNormalization(float coefficient, float damping) {
        return 1.f / (1.f + coefficient * (coefficient + damping));
    }
    static float focusDampingFor(float gain) {
        return (1.f / 1.15f) / std::sqrt(1.f + gain);
    }
    float cutoffCoefficient(float hz) const { return std::tan(3.14159265f * std::min(hz / rate, .45f)); }
    float time(float seconds) const { return 1.f - std::exp(-1.f / (rate * seconds)); }
    float filter(float hz) const { return time(1.f / (6.2831853f * hz)); }

    std::array<Channel, 2> channels;
    std::array<Room, 2> rooms;
    CachedCoefficient cutoffCache, focusFrequencyCache, focusGainCache, inputGainCache, outputGainCache;
    float cachedDecay = std::numeric_limits<float>::infinity();
    float rate = 48000, lowCutG = 0, presence = 0, air = 0, sibilance = 0;
    bool initialized = false;
    float wetLow = 0, wetHigh = 0, roomDamping = 0, fast = 0, release = 0, smooth = 0;
    float noiseOpen = 0, colourPole = 0, switchStep = 0;
    float compAttack = 0, compRelease = 0, floorRise = 0, floorFall = 0, duckRelease = 0;
    float compPower = 0;
    float noiseGain = 1, deessGain = 0, compGain = 1, duckGain = 1;
    float lowCutMix = 1, focusG = 0, focusGain = 0;
    float tone = 0, wet = 0, active = 1, inputGain = 1, outGain = 1;
};

} // namespace ratbox
