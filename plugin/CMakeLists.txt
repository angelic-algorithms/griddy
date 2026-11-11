cmake_minimum_required(VERSION 3.22)

# Must be set before project() so it applies to every target.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
# Universal binary for Intel and Apple Silicon Macs. Override on the command line if you only want one.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

project(Griddy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    enable_language(OBJCXX)
endif()

option(GRIDDY_BUILD_TESTS "Build the GriddyTests console app" ON)
option(GRIDDY_UI_DEV_SERVER "Load the UI from the Vite/esbuild dev server (http://localhost:5173) for hot reload" OFF)

# ----------------------------------------------------------------------------
# Dependencies. Pinned to exact tags so builds are reproducible.
# Set JUCE_DIR to a local checkout to skip the download.
# ----------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/_deps/juce-build)
else()
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# ----------------------------------------------------------------------------
# The engine core: take recording, history, decoding, sync, playback. No plugin or UI code,
# so the tests and the headless harness build exactly the same sources the plugin does.
# ----------------------------------------------------------------------------
set(GRIDDY_CORE_SOURCES
    source/engine/BeatClock.cpp
    source/engine/CollageLayout.cpp
    source/engine/Compositor.cpp
    source/engine/FrameStore.cpp
    source/engine/TakeFormat.cpp
    source/engine/TakeHistory.cpp
    source/engine/TakeRecorder.cpp
    source/engine/VideoDecoder.cpp
    source/engine/SyncController.cpp
    source/engine/SlotPlayer.cpp
    source/engine/PlaybackEngine.cpp
    source/engine/sync/EmbeddedSync.cpp
    source/engine/sync/Fingerprint.cpp)

# ----------------------------------------------------------------------------
# The plugin
# ----------------------------------------------------------------------------
juce_add_plugin(Griddy
    COMPANY_NAME "Park Angel"
    BUNDLE_ID "com.parkangel.griddy"
    PLUGIN_MANUFACTURER_CODE Pkag
    PLUGIN_CODE Grdy
    PRODUCT_NAME "Griddy"
    FORMATS VST3 AU Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect
    # These only reach the Standalone app's Info.plist. A plugin runs inside the
    # host, so the host's own Info.plist and entitlements decide camera access.
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "Griddy uses your cameras to record a video collage in time with your music."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.camera)

target_sources(Griddy PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/UiResources.cpp
    ${GRIDDY_CORE_SOURCES}
    source/engine/VideoEngine.cpp
    source/capture/TestPatternSource.cpp
    source/capture/CaptureDevices.cpp)

if(APPLE)
    target_sources(Griddy PRIVATE
        source/capture/AVFoundationCapture.mm
        source/capture/HostPermissions_mac.mm)
    # Our Objective-C++ files use ARC so we never hand manage retain/release.
    set_source_files_properties(
        source/capture/AVFoundationCapture.mm
        source/capture/HostPermissions_mac.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(Griddy PRIVATE
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Security")
else()
    target_sources(Griddy PRIVATE source/capture/HostPermissions_other.cpp)
endif()

target_include_directories(Griddy PRIVATE source)

target_compile_definitions(Griddy PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    GRIDDY_UI_DEV_SERVER=$<BOOL:${GRIDDY_UI_DEV_SERVER}>)

# ----------------------------------------------------------------------------
# The web UI. Build it first with `npm ci && npm run build` inside ui/.
# Every file in ui/dist is embedded into the plugin binary, so the plugin
# never needs a web server or files next to it.
# ----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/index.html")
    find_program(NPM_EXECUTABLE npm)
    if(NPM_EXECUTABLE)
        message(STATUS "Building the web UI (first configure only)...")
        execute_process(COMMAND ${NPM_EXECUTABLE} ci WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
        execute_process(COMMAND ${NPM_EXECUTABLE} run build WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui)
    endif()
endif()
file(GLOB_RECURSE GRIDDY_UI_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/*")
if(NOT GRIDDY_UI_FILES)
    message(WARNING "ui/dist is empty. Run `npm ci && npm run build` in ui/ first. Building with the fallback page only.")
endif()
juce_add_binary_data(GriddyUiData
    HEADER_NAME GriddyUiData.h
    NAMESPACE GriddyUiData
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ui/fallback.html ${GRIDDY_UI_FILES})

target_link_libraries(Griddy
    PRIVATE
        GriddyUiData
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_video
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ----------------------------------------------------------------------------
# Tests: the engine pieces that don't need a camera or a DAW.
# ----------------------------------------------------------------------------
if(GRIDDY_BUILD_TESTS)
    juce_add_console_app(GriddyTests PRODUCT_NAME "GriddyTests")
    target_sources(GriddyTests PRIVATE
        tests/TestMain.cpp
        tests/EngineTests.cpp
        tests/EngineSyncTests.cpp
        ${GRIDDY_CORE_SOURCES}
        source/capture/TestPatternSource.cpp)
    target_include_directories(GriddyTests PRIVATE source)
    target_compile_definitions(GriddyTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(GriddyTests
        PRIVATE juce::juce_core juce::juce_graphics juce::juce_events juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # A fake DAW that loads the built VST3 and exercises it end to end.
    juce_add_console_app(GriddyHostSmokeTest PRODUCT_NAME "GriddyHostSmokeTest")
    target_sources(GriddyHostSmokeTest PRIVATE tests/HostSmokeTest.cpp)
    target_compile_definitions(GriddyHostSmokeTest PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(GriddyHostSmokeTest
        PRIVATE juce::juce_audio_processors juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_dependencies(GriddyHostSmokeTest Griddy_VST3)

    enable_testing()
    add_test(NAME GriddyTests COMMAND GriddyTests)
    if(APPLE)
        set(GRIDDY_VST3_PATH "$<TARGET_BUNDLE_DIR:Griddy_VST3>")
    else() # Griddy.vst3/Contents/<arch>/Griddy.so
        set(GRIDDY_VST3_PATH "$<TARGET_FILE_DIR:Griddy_VST3>/../..")
    endif()
    add_test(NAME GriddyHostSmokeTest COMMAND GriddyHostSmokeTest ${GRIDDY_VST3_PATH})
endif()
