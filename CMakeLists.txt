cmake_minimum_required(VERSION 3.22)
project(OpenSection VERSION 0.1.7 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OPENSECTION_TOOLS "Build command-line renderer / benchmark" ON)

add_library(opensection_engine STATIC
  engine/src/Engine.cpp
  engine/src/Model.cpp
  engine/src/BuiltinModel.cpp)
target_include_directories(opensection_engine PUBLIC engine/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(opensection_engine PRIVATE -O3 -ffast-math -Wall -Wextra)
elseif(MSVC)
  target_compile_options(opensection_engine PRIVATE /O2 /fp:fast)
endif()
target_compile_definitions(opensection_engine PUBLIC _USE_MATH_DEFINES)
set_target_properties(opensection_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OPENSECTION_TOOLS)
  add_executable(opensection-render tools/render/main.cpp)
  target_link_libraries(opensection-render PRIVATE opensection_engine)
endif()

# ------------------------------------------------------------------ plugin
option(OPENSECTION_PLUGIN "Build the plugin (VST3 / CLAP / LV2 / AU / Standalone)" ON)
if(OPENSECTION_PLUGIN)
  add_subdirectory(libs/JUCE EXCLUDE_FROM_ALL)
  set(OS_FORMATS VST3 LV2 Standalone)
  if(APPLE)
    list(APPEND OS_FORMATS AU)
  endif()
  juce_add_plugin(OpenSection
    PRODUCT_NAME "OpenSection"
    COMPANY_NAME "OpenSection"
    COMPANY_WEBSITE "https://github.com/doitux/OpenSection"
    BUNDLE_ID "io.github.doitux.opensection"
    PLUGIN_MANUFACTURER_CODE Opsn
    PLUGIN_CODE OpSc
    FORMATS ${OS_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    LV2URI "https://github.com/doitux/OpenSection"
    COPY_PLUGIN_AFTER_BUILD FALSE)
  target_sources(OpenSection PRIVATE plugin/Processor.cpp plugin/Editor.cpp)
  target_compile_definitions(OpenSection PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
  target_link_libraries(OpenSection PRIVATE opensection_engine
    juce::juce_audio_utils juce::juce_audio_processors
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

  add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)
  clap_juce_extensions_plugin(TARGET OpenSection
    CLAP_ID "io.github.doitux.opensection"
    CLAP_FEATURES instrument synthesizer stereo)
endif()
