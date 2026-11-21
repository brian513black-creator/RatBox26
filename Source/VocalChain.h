#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ratbox {

struct Settings {
    float noise = .45f, deess = .30f, character = .32f, comp = .34f;
    float space = .15f, decay = 1.65f, duck = .65f, output = 0.f;
    bool bypass = false;
    float lowCut = 70.f, focusHz = 280.f, focusDb = 0.f;
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
        wetLow = filter(220);
        wetHigh = filter(6100);
        roomDamping = filter(2830);
        fast = time(.003f);
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
        levelPower = highPower = compPower = 0.f;
        floor = .0015f;
        noiseGain = compGain = duckGain = 1.f;
        deessGain = 0.f;
        initialized = false;
    }

    Meters process(float* left, float* right, int frames, const Settings& target) {
        Meters meters;
        std::array<std::array<float, 4>, 2> feedback{};
        for (size_t channel = 0; channel < rooms.size(); ++channel)
            for (size_t i = 0; i < rooms[channel].combs.size(); ++i)
                feedback[channel][i] = std::pow(.001f, static_cast<float>(
                    rooms[channel].combs[i].samples.size()) / (rate * target.decay));

        const float cutoffTarget = cutoffCoefficient(target.lowCut);
        const float focusTarget = cutoffCoefficient(target.focusHz);
        const float focusGainTarget = std::pow(10.f, target.focusDb / 20.f) - 1.f;
        const float outTarget = std::pow(10.f, target.output / 20.f);
        if (!initialized) {
            lowCutG = cutoffTarget;
            focusG = focusTarget;
            focusGain = focusGainTarget;
            tone = target.character;
            wet = target.space;
            outGain = outTarget;
            active = target.bypass ? 0.f : 1.f;
            for (size_t channel = 0; channel < rooms.size(); ++channel)
                for (size_t i = 0; i < rooms[channel].combs.size(); ++i)
                    rooms[channel].combs[i].feedback = feedback[channel][i];
            initialized = true;
        }
        for (int frame = 0; frame < frames; ++frame) {
            float dry[2] = {safe(left[frame]), right ? safe(right[frame]) : safe(left[frame])};
            float clean[2], high[2];
            follow(lowCutG, cutoffTarget, smooth);
            follow(focusG, focusTarget, smooth);
            follow(focusGain, focusGainTarget, smooth);
            constexpr float focusDamping = 1.f / 1.15f;
            const float focusA1 = 1.f / (1.f + focusG * (focusG + focusDamping));
            constexpr float damping = 1.41421356f;
            const float a1 = 1.f / (1.f + lowCutG * (lowCutG + damping));
            for (size_t channel = 0; channel < 2; ++channel) {
                auto& state = channels[channel];
                const float v1 = a1 * (state.integrator1 + lowCutG * (dry[channel] - state.integrator2));
                const float v2 = state.integrator2 + lowCutG * v1;
                state.integrator1 = 2.f * v1 - state.integrator1;
                state.integrator2 = 2.f * v2 - state.integrator2;
                clean[channel] = dry[channel] - damping * v1 - v2;
                const float sibV = (clean[channel] - state.sib) * sibilance;
                const float sibLow = state.sib + sibV;
                state.sib = sibLow + sibV;
                high[channel] = clean[channel] - sibLow;
            }

            const float input = std::max(std::abs(dry[0]), std::abs(dry[1]));
            const float level = std::max(std::abs(clean[0]), std::abs(clean[1]));
            const float hf = std::max(std::abs(high[0]), std::abs(high[1]));
            envelope(levelPower, level * level, fast, release);
            envelope(highPower, hf * hf, fast, release);
            const float voiceLevel = std::sqrt(levelPower);
            const float highLevel = std::sqrt(highPower);

            // Estimate stationary background from quiet passages and freeze on speech.
            const float candidate = std::clamp(voiceLevel, .0002f, .035f);
            if (candidate < floor || voiceLevel < std::max(.016f, floor * 2.4f))
                follow(floor, candidate, candidate < floor ? floorFall : floorRise);
            const float snr = voiceLevel / std::max(floor, .0015f);
            const float speech = std::clamp((snr - 1.25f) / 2.25f, 0.f, 1.f);
            const float noiseTarget = 1.f - .95f * target.noise * (1.f - speech * speech * (3.f - 2.f * speech));
            follow(noiseGain, noiseTarget, noiseTarget > noiseGain ? fast : release);

            const float sRatio = highLevel / std::max(voiceLevel, .008f);
            const float sReduction = target.deess * .78f *
                std::clamp((sRatio - .28f) * 2.6f, 0.f, 1.f) *
                std::clamp((highLevel * noiseGain - .012f) * 65.f, 0.f, 1.f);
            follow(deessGain, sReduction, sReduction > deessGain ? fast : release);

            follow(tone, target.character, smooth);
            follow(wet, target.space, smooth);
            follow(outGain, outTarget, smooth);
            follow(active, target.bypass ? 0.f : 1.f, fast);

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
                const float drive = 1.f + .65f * tone;
                const float colored = std::tanh(x * drive) / drive;
                clean[channel] = x + .4f * tone * (colored - x);
            }

            const float shaped = std::max(std::abs(clean[0]), std::abs(clean[1]));
            envelope(compPower, shaped * shaped, fast, release);
            const float compLevel = std::sqrt(compPower);
            const float threshold = .25f - .175f * target.comp;
            const float over = std::max(1.f, compLevel / threshold);
            const float compTarget = std::pow(over, -(.64f * target.comp));
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
                for (size_t i = 0; i < room.combs.size(); ++i) {
                    auto& comb = room.combs[i];
                    const float previous = comb.read();
                    follow(comb.damped, previous, roomDamping);
                    follow(comb.feedback, feedback[channel][i], smooth);
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
            left[frame] = dry[0] + (stereoOrMono - dry[0]) * active;
            if (right)
                right[frame] = dry[1] + (clean[1] - dry[1]) * active;
            meters.input = std::max(meters.input, input);
            meters.output = std::max(meters.output, std::max(std::abs(left[frame]),
                right ? std::abs(right[frame]) : 0.f));
            meters.noise = std::max(meters.noise, (1.f - noiseGain) * active);
            meters.comp = std::max(meters.comp, (1.f - compGain) * active);
        }
        return meters;
    }

private:
    struct Delay {
        std::vector<float> samples;
        size_t cursor = 0;
        float damped = 0, feedback = 0;
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
    };

    static float safe(float x) { return std::isfinite(x) ? std::clamp(x, -64.f, 64.f) : 0.f; }
    static float follow(float& state, float value, float coefficient) {
        state += coefficient * (value - state);
        return state;
    }
    static void envelope(float& state, float value, float attack, float release) {
        follow(state, value, value > state ? attack : release);
    }
    float cutoffCoefficient(float hz) const { return std::tan(3.14159265f * std::min(hz / rate, .45f)); }
    float time(float seconds) const { return 1.f - std::exp(-1.f / (rate * seconds)); }
    float filter(float hz) const { return time(1.f / (6.2831853f * hz)); }

    std::array<Channel, 2> channels;
    std::array<Room, 2> rooms;
    float rate = 48000, lowCutG = 0, presence = 0, air = 0, sibilance = 0;
    bool initialized = false;
    float wetLow = 0, wetHigh = 0, roomDamping = 0, fast = 0, release = 0, smooth = 0;
    float compAttack = 0, compRelease = 0, floorRise = 0, floorFall = 0, duckRelease = 0;
    float levelPower = 0, highPower = 0, compPower = 0, floor = .0015f;
    float noiseGain = 1, deessGain = 0, compGain = 1, duckGain = 1;
    float focusG = 0, focusGain = 0;
    float tone = 0, wet = 0, active = 1, outGain = 1;
};

} // namespace ratbox
