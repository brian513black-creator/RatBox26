#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
 #define NOMINMAX
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

void check(bool condition, const char* stage) {
    if (condition) return;
    std::cerr << "VST3 smoke failed at " << stage << '\n';
    std::exit(1);
}

int main(int argc, char** argv) {
    check(argc == 2, "argument");
#ifdef _WIN32
    const auto library = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
    check(library != nullptr, "load");
    const auto symbol = [library](const char* name) { return GetProcAddress(library, name); };
    const auto entry = reinterpret_cast<bool (*)()>(symbol("InitDll"));
    const auto exitFn = reinterpret_cast<bool (*)()>(symbol("ExitDll"));
    check(entry && exitFn && entry(), "module entry");
#else
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) std::cerr << dlerror() << '\n';
    check(library != nullptr, "load");
    const auto symbol = [library](const char* name) { return dlsym(library, name); };
    const auto entry = reinterpret_cast<bool (*)(void*)>(symbol("ModuleEntry"));
    const auto exitFn = reinterpret_cast<bool (*)()>(symbol("ModuleExit"));
    check(entry && exitFn && entry(library), "module entry");
#endif
    const auto factoryFn = reinterpret_cast<IPluginFactory* (PLUGIN_API*)()>(symbol("GetPluginFactory"));
    check(factoryFn != nullptr, "factory symbol");
    auto* factory = factoryFn();
    check(factory != nullptr, "factory");
    for (const auto layout : {std::array<int, 2>{1, 1}, {2, 2}, {1, 2}}) {
        const auto inputChannels = layout[0];
        const auto outputChannels = layout[1];
        IComponent* component = nullptr;
        for (int index = 0; index < factory->countClasses(); ++index) {
            PClassInfo info{};
            if (factory->getClassInfo(index, &info) == kResultOk &&
                std::strcmp(info.category, kVstAudioEffectClass) == 0)
                check(factory->createInstance(info.cid, INLINE_UID_OF(IComponent),
                    reinterpret_cast<void**>(&component)) == kResultOk, "create component");
        }
        check(component != nullptr && component->initialize(nullptr) == kResultOk, "initialize");
        IAudioProcessor* processor = nullptr;
        check(component->queryInterface(INLINE_UID_OF(IAudioProcessor),
            reinterpret_cast<void**>(&processor)) == kResultOk, "audio interface");
        auto inArrangement = inputChannels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        auto outArrangement = outputChannels == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        check(processor->setBusArrangements(&inArrangement, 1, &outArrangement, 1) == kResultOk,
            "set audio bus arrangement");
        check(component->activateBus(kAudio, kInput, 0, true) == kResultOk, "activate input bus");
        check(component->activateBus(kAudio, kOutput, 0, true) == kResultOk, "activate output bus");
        ProcessSetup setup{kRealtime, kSample32, 256, 48000.0};
        check(processor->setupProcessing(setup) == kResultOk, "setup process");

        std::array<float, 256> l{}, r{}, outL{}, outR{};
        float* inputs[]{l.data(), r.data()};
        float* outputs[]{outL.data(), outR.data()};
        AudioBusBuffers input, output;
        input.numChannels = inputChannels;
        output.numChannels = outputChannels;
        input.channelBuffers32 = inputs;
        output.channelBuffers32 = outputs;
        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = 256;
        data.numInputs = data.numOutputs = 1;
        data.inputs = &input;
        data.outputs = &output;
        const auto checkInactiveAudio = [&] {
            l.fill(.12f);
            r.fill(.23f);
            input.silenceFlags = 0;
            check(processor->process(data) == kResultOk, "process while inactive");
            for (int ch = 0; ch < outputChannels; ++ch)
                for (int i = 0; i < 256; ++i)
                    check(outputs[ch][i] == (ch == 1 && inputChannels == 2 ? .23f : .12f),
                          "inactive processing should pass dry audio");
        };
        checkInactiveAudio();

        check(component->setActive(true) == kResultOk, "activate");
        check(processor->setProcessing(true) == kResultOk, "start processing");
        std::array<float, 2> energy{};
        float stereoDifference = 0.f;
        for (int b = 0; b < 25; ++b) {
            for (int i = 0; i < 256; ++i)
                l[i] = r[i] = .2f * std::sin((b * 256 + i) * .0576f);
            check(processor->process(data) == kResultOk, "process block");
            for (int ch = 0; ch < outputChannels; ++ch)
                for (int i = 0; i < 256; ++i) {
                    check(std::isfinite(outputs[ch][i]), "finite output");
                    energy[ch] += outputs[ch][i] * outputs[ch][i];
                }
            if (outputChannels == 2)
                for (int i = 0; i < 256; ++i)
                    stereoDifference += std::abs(outL[i] - outR[i]);
        }
        for (int ch = 0; ch < outputChannels; ++ch)
            check(energy[ch] > 20.f, "signal through each output channel");
        if (outputChannels == 2) check(stereoDifference > .1f, "stereo reverb");

        check(processor->setProcessing(false) == kResultOk, "stop processing");
        check(processor->setProcessing(true) == kResultOk, "restart processing");
        l.fill(0.f);
        r.fill(0.f);
        input.silenceFlags = (1u << inputChannels) - 1u;
        for (int b = 0; b < 12; ++b) {
            check(processor->process(data) == kResultOk, "process silence after restart");
            for (int ch = 0; ch < outputChannels; ++ch)
                for (int i = 0; i < 256; ++i)
                    check(outputs[ch][i] == 0.f, "reset clears previous audio");
        }
        processor->setProcessing(false);
        check(component->setActive(false) == kResultOk, "deactivate");
        checkInactiveAudio();
        processor->release();
        component->terminate();
        component->release();
    }
    factory->release();
    check(exitFn(), "module exit");
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    std::cout << "VST3 load, mono, stereo, and mono-to-stereo audio processing passed\n";
}
