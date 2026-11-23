#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
 #define NOMINMAX
 #include <windows.h>
#elif defined(__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
#else
 #include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void check(bool condition, const char* stage) {
    if (condition) return;
    std::cerr << "VST3 smoke failed at " << stage << '\n';
    std::exit(1);
}

IComponent* createComponent(IPluginFactory& factory) {
    for (int index = 0; index < factory.countClasses(); ++index) {
        PClassInfo info{};
        if (factory.getClassInfo(index, &info) != kResultOk ||
            std::strcmp(info.category, kVstAudioEffectClass) != 0)
            continue;
        IComponent* component = nullptr;
        check(factory.createInstance(info.cid, INLINE_UID_OF(IComponent),
            reinterpret_cast<void**>(&component)) == kResultOk, "create component");
        check(component != nullptr && component->initialize(nullptr) == kResultOk, "initialize component");
        return component;
    }
    check(false, "find audio component");
    return nullptr;
}

template <class Interface, const TUID& InterfaceId>
class TestHostObject : public Interface {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override {
        if (std::memcmp(iid, InterfaceId, sizeof(TUID)) == 0 ||
            std::memcmp(iid, INLINE_UID_OF(FUnknown), sizeof(TUID)) == 0) {
            *object = static_cast<Interface*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    // The host test owns these objects and keeps them alive across plugin calls.
    uint32 PLUGIN_API addRef() override { return ++references; }
    uint32 PLUGIN_API release() override { return --references; }

private:
    uint32 references = 1;
};

class StateStream final : public TestHostObject<IBStream, INLINE_UID_OF(IBStream)> {
public:
    tresult PLUGIN_API read(void* buffer, int32 bytes, int32* readBytes) override {
        if (bytes < 0) return kInvalidArgument;
        const auto count = std::min(static_cast<size_t>(bytes), data.size() - position);
        if (count != 0) std::memcpy(buffer, data.data() + position, count);
        position += count;
        if (readBytes != nullptr) *readBytes = static_cast<int32>(count);
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 bytes, int32* writtenBytes) override {
        if (bytes < 0) return kInvalidArgument;
        const auto end = position + static_cast<size_t>(bytes);
        if (end > data.size()) data.resize(end);
        if (bytes != 0) std::memcpy(data.data() + position, buffer, static_cast<size_t>(bytes));
        position = end;
        if (writtenBytes != nullptr) *writtenBytes = bytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 offset, int32 mode, int64* result) override {
        if (mode == kIBSeekCur) offset += static_cast<int64>(position);
        else if (mode == kIBSeekEnd) offset += static_cast<int64>(data.size());
        else if (mode != kIBSeekSet) return kInvalidArgument;
        if (offset < 0 || offset > static_cast<int64>(data.size())) return kInvalidArgument;
        position = static_cast<size_t>(offset);
        if (result != nullptr) *result = offset;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* result) override {
        *result = static_cast<int64>(position);
        return kResultOk;
    }

private:
    std::vector<unsigned char> data;
    size_t position = 0;
};

class ParameterQueue final : public TestHostObject<IParamValueQueue, INLINE_UID_OF(IParamValueQueue)> {
public:
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }

    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& result) override {
        if (index != 0) return kInvalidArgument;
        offset = 0;
        result = value;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }

    ParamID id{};
    ParamValue value{};
};

constexpr int parameterCount = 14;

class ParameterChanges final : public TestHostObject<IParameterChanges, INLINE_UID_OF(IParameterChanges)> {
public:
    int32 PLUGIN_API getParameterCount() override { return parameterCount; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || index >= parameterCount) return nullptr;
        return &queues[static_cast<size_t>(index)];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }

    std::array<ParameterQueue, parameterCount> queues;
};

class ComponentAndController {
public:
    explicit ComponentAndController(IPluginFactory& factory) : component(createComponent(factory)) {
        TUID controllerId{};
        check(component->getControllerClassId(controllerId) == kResultOk, "get controller class ID");
        check(factory.createInstance(controllerId, INLINE_UID_OF(IEditController),
            reinterpret_cast<void**>(&controller)) == kResultOk, "create controller");
        check(controller->initialize(nullptr) == kResultOk, "initialize controller");

        check(component->queryInterface(INLINE_UID_OF(IConnectionPoint),
            reinterpret_cast<void**>(&componentConnection)) == kResultOk, "component connection interface");
        check(controller->queryInterface(INLINE_UID_OF(IConnectionPoint),
            reinterpret_cast<void**>(&controllerConnection)) == kResultOk, "controller connection interface");
        check(componentConnection->connect(controllerConnection) == kResultOk, "connect component to controller");
        check(controllerConnection->connect(componentConnection) == kResultOk, "connect controller to component");
    }

    ~ComponentAndController() {
        controllerConnection->disconnect(componentConnection);
        componentConnection->disconnect(controllerConnection);
        controllerConnection->release();
        componentConnection->release();
        controller->terminate();
        component->terminate();
        controller->release();
        component->release();
    }

    IComponent* const component;
    IEditController* controller = nullptr;

private:
    IConnectionPoint* componentConnection = nullptr;
    IConnectionPoint* controllerConnection = nullptr;
};

std::string parameterText(const String128& text) {
    std::string result;
    for (const auto character : text) {
        if (character == 0) break;
        result += static_cast<char>(character);
    }
    return result;
}

struct NumericParameter {
    const char* title;
    const char* unit;
    double minimum, maximum, initial;
};

constexpr std::array<NumericParameter, parameterCount - 2> numericParameters{{
    {"Background Noise", "%", 0., 100., 45.},
    {"De-ess", "%", 0., 100., 30.},
    {"Character", "%", 0., 100., 32.},
    {"Compression", "%", 0., 100., 34.},
    {"Reverb Level", "%", 0., 100., 15.},
    {"Decay", "s", .35, 5., 1.65},
    {"Vocal Ducking", "%", 0., 100., 65.},
    {"Output", "dB", -18., 12., 0.},
    {"Low Cut", "Hz", 70., 350., 70.},
    {"Focus Frequency", "Hz", 150., 500., 280.},
    {"Focus Gain", "dB", -6., 6., 0.},
    {"Input Trim", "dB", -18., 18., 0.}
}};

void testNumericParameter(IEditController& controller, const ParameterInfo& info) {
    const auto title = parameterText(info.title);
    const auto contract = std::find_if(numericParameters.begin(), numericParameters.end(),
        [&](const NumericParameter& parameter) { return title == parameter.title; });
    check(contract != numericParameters.end(), "host numeric parameter title");
    check(parameterText(info.units) == contract->unit, "host unit metadata");

    const double normalized[]{0., 1., info.defaultNormalizedValue};
    const double expected[]{contract->minimum, contract->maximum, contract->initial};
    for (size_t i = 0; i < std::size(normalized); ++i) {
        String128 text{};
        check(controller.getParamStringByValue(info.id, normalized[i], text) == kResultOk,
              "format host parameter value");
        const auto display = parameterText(text);
        char* end = nullptr;
        const auto number = std::strtod(display.c_str(), &end);
        check(end != display.c_str() && *end == 0 && std::abs(number - expected[i]) < .00001,
              "rendered parameter default and endpoints");
        ParamValue parsed{};
        check(controller.getParamValueByString(info.id, text, parsed) == kResultOk &&
              std::abs(parsed - normalized[i]) < .00001, "parse rendered host parameter value");
    }

    String128 invalidValues[]{ {'b', 'a', 'd'}, {'1', 'b', 'a', 'd'}, {'n', 'a', 'n'}, {'i', 'n', 'f'} };
    for (auto& invalid : invalidValues) {
        ParamValue parsed{};
        check(controller.getParamValueByString(info.id, invalid, parsed) != kResultOk ||
              std::abs(parsed - info.defaultNormalizedValue) < .00001,
              "invalid host parameter text leaves current value unchanged");
    }
}

void testHostParameters(IPluginFactory& factory) {
    ComponentAndController source(factory), restored(factory);
    check(source.controller->getParameterCount() == parameterCount, "all parameters exposed to host");

    ParameterChanges automation;
    int bypassCount = 0, lowCutSwitchCount = 0;
    for (int i = 0; i < parameterCount; ++i) {
        ParameterInfo info{};
        check(source.controller->getParameterInfo(i, info) == kResultOk, "get parameter info");
        check(info.title[0] != 0 && info.shortTitle[0] != 0, "parameter names exposed");
        check((info.flags & ParameterInfo::kCanAutomate) != 0, "parameter is automatable");
        check(info.defaultNormalizedValue >= 0. && info.defaultNormalizedValue <= 1.,
              "parameter default is normalized");
        if ((info.flags & ParameterInfo::kIsBypass) != 0) {
            ++bypassCount;
            check(info.stepCount == 1 && info.defaultNormalizedValue == 0., "bypass is a default-off toggle");
        } else if (parameterText(info.title) == "Low Cut Enabled") {
            ++lowCutSwitchCount;
            check(info.stepCount == 1 && info.defaultNormalizedValue == 1., "low cut is a default-on toggle");
        } else {
            testNumericParameter(*source.controller, info);
        }
        auto& queue = automation.queues[static_cast<size_t>(i)];
        queue.id = info.id;
        queue.value = info.defaultNormalizedValue < .5 ? 1. : 0.;
        check(std::none_of(automation.queues.begin(), automation.queues.begin() + i,
            [&](const ParameterQueue& previous) { return previous.id == queue.id; }),
            "parameter ID is unique");
        check(restored.controller->getParamNormalized(queue.id) == info.defaultNormalizedValue,
              "fresh instance starts at default values");
    }
    check(bypassCount == 1, "exactly one host bypass");
    check(lowCutSwitchCount == 1, "exactly one Low Cut Enabled parameter");

    IAudioProcessor* processor = nullptr;
    check(source.component->queryInterface(INLINE_UID_OF(IAudioProcessor),
        reinterpret_cast<void**>(&processor)) == kResultOk, "automation processor interface");
    ProcessData data;
    data.inputParameterChanges = &automation;
    check(processor->process(data) == kResultOk, "host automation flush");
    processor->release();

    StateStream state;
    check(source.component->getState(&state) == kResultOk, "save component state");
    check(state.seek(0, IBStream::kIBSeekSet, nullptr) == kResultOk, "rewind saved component state");
    check(restored.component->setState(&state) == kResultOk, "restore component state");
    check(state.seek(0, IBStream::kIBSeekSet, nullptr) == kResultOk, "rewind for controller restore");
    check(restored.controller->setComponentState(&state) == kResultOk, "sync restored controller state");
    for (const auto& queue : automation.queues)
        check(restored.controller->getParamNormalized(queue.id) == queue.value,
              "host-automated value restored");
}

void testHostEditor(IPluginFactory& factory) {
    ComponentAndController host(factory);
    auto* view = host.controller->createView(ViewType::kEditor);
    check(view != nullptr, "VST3 host creates editor");
#ifdef _WIN32
    check(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultOk,
          "VST3 editor supports HWND embedding");
#elif defined(__APPLE__)
    check(view->isPlatformTypeSupported(kPlatformTypeNSView) == kResultOk,
          "VST3 editor supports NSView embedding");
#else
    check(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultOk,
          "VST3 editor supports X11 embedding");
#endif
    check(view->canResize() == kResultOk, "VST3 editor exposes resize");
    ViewRect limits{0, 0, 100, 100};
    check(view->checkSizeConstraint(&limits) == kResultOk, "VST3 editor reports minimum size");
    ViewRect requested{0, 0, limits.getWidth() + 20, limits.getHeight() + 20};
    ViewRect actual{};
    check(view->onSize(&requested) == kResultOk && view->getSize(&actual) == kResultOk,
          "VST3 editor accepts host resize");
#ifdef __APPLE__
    // NSView sizes use logical points. Allow rounding on conversion back to host bounds.
    constexpr int tolerance = 1;
#else
    constexpr int tolerance = 0;
#endif
    check(std::abs(actual.getWidth() - requested.getWidth()) <= tolerance &&
          std::abs(actual.getHeight() - requested.getHeight()) <= tolerance,
          "VST3 editor applies host resize");
    view->release();
    view = host.controller->createView(ViewType::kEditor);
    check(view != nullptr, "VST3 editor reopens");
    view->release();
}

} // namespace

int main(int argc, char** argv) {
    check(argc == 2, "argument");
#ifdef _WIN32
    const auto library = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
    check(library != nullptr, "load");
    const auto symbol = [library](const char* name) { return GetProcAddress(library, name); };
    const auto entry = reinterpret_cast<bool (*)()>(symbol("InitDll"));
    const auto exitFn = reinterpret_cast<bool (*)()>(symbol("ExitDll"));
    check(entry && exitFn && entry(), "module entry");
#elif defined(__APPLE__)
    const auto path = std::filesystem::absolute(argv[1]).native();
    const auto url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), true);
    check(url != nullptr, "create VST3 bundle URL");
    const auto library = CFBundleCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    check(library != nullptr, "create VST3 bundle");
    CFErrorRef loadError = nullptr;
    const auto loaded = CFBundleLoadExecutableAndReturnError(library, &loadError);
    if (loadError != nullptr) {
        CFShow(loadError);
        CFRelease(loadError);
    }
    check(loaded, "load VST3 bundle");
    const auto symbol = [library](const char* name) {
        const auto symbolName = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingASCII);
        check(symbolName != nullptr, "create VST3 symbol name");
        const auto address = CFBundleGetFunctionPointerForName(library, symbolName);
        CFRelease(symbolName);
        return address;
    };
    const auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(symbol("bundleEntry"));
    const auto exitFn = reinterpret_cast<bool (*)()>(symbol("bundleExit"));
    check(entry && exitFn && entry(library), "module entry");
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
        auto* component = createComponent(*factory);
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
    testHostParameters(*factory);
    testHostEditor(*factory);
    factory->release();
    check(exitFn(), "module exit");
#ifdef _WIN32
    FreeLibrary(library);
#elif defined(__APPLE__)
    // Match the Steinberg host: release the bundle after bundleExit, with no forced unload.
    CFRelease(library);
#else
    dlclose(library);
#endif
    std::cout << "VST3 audio, automation, state restore, and editor lifecycle passed\n";
}
