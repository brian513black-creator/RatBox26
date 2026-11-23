cmake_minimum_required(VERSION 3.22)
project(RatBox26 VERSION 0.1.4 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ratbox-version.txt" CONTENT "${PROJECT_VERSION}\n")

include(CTest)
if(BUILD_TESTING)
    add_executable(ratbox_dsp_tests tests/DspTests.cpp)
    target_include_directories(ratbox_dsp_tests PRIVATE Source)
    add_test(NAME ratbox_dsp_tests COMMAND ratbox_dsp_tests)
endif()

option(RATBOX_BUILD_PLUGIN "Build the RatBox26 plugin bundles" ON)
if(RATBOX_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.6
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
    set(ratbox_formats VST3)
    set(ratbox_editor_keyboard_focus FALSE)
    if(APPLE)
        list(APPEND ratbox_formats AU)
        set(ratbox_editor_keyboard_focus TRUE)
    endif()
    juce_add_plugin(RatBox26
        COMPANY_NAME "RatBox Audio"
        PLUGIN_MANUFACTURER_CODE RtbA
        PLUGIN_CODE Rb26
        BUNDLE_ID com.ratboxaudio.ratbox26
        PRODUCT_NAME "RatBox26"
        EDITOR_WANTS_KEYBOARD_FOCUS ${ratbox_editor_keyboard_focus}
        FORMATS ${ratbox_formats})
    target_sources(RatBox26 PRIVATE Source/Plugin.cpp)
    target_compile_definitions(RatBox26 PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(RatBox26 PRIVATE juce::juce_audio_utils
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    if(BUILD_TESTING AND (APPLE OR WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        add_executable(ratbox_vst3_smoke tests/Vst3Smoke.cpp)
        target_include_directories(ratbox_vst3_smoke PRIVATE
            "${juce_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
        if(APPLE)
            find_library(ratbox_core_foundation CoreFoundation REQUIRED)
            target_link_libraries(ratbox_vst3_smoke PRIVATE "${ratbox_core_foundation}")
            set(ratbox_vst3_test_module "$<TARGET_BUNDLE_DIR:RatBox26_VST3>")
        else()
            target_link_libraries(ratbox_vst3_smoke PRIVATE ${CMAKE_DL_LIBS})
            set(ratbox_vst3_test_module "$<TARGET_FILE:RatBox26_VST3>")
        endif()
        add_dependencies(ratbox_vst3_smoke RatBox26_VST3)
        add_test(NAME ratbox_vst3_smoke COMMAND ratbox_vst3_smoke "${ratbox_vst3_test_module}")
    endif()
endif()
