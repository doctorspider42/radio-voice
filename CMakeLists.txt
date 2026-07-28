cmake_minimum_required(VERSION 3.24)

# The version is written down exactly once, in VERSION at the repository root.
# Everything that needs it - the executable's resource block, the installer, the
# release tag - reads it from here, so a bump cannot be applied to three places
# and forgotten in the fourth.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION RV_VERSION_FILE)
string(STRIP "${RV_VERSION_FILE}" RV_VERSION_FILE)

project(RadioVoice
    VERSION ${RV_VERSION_FILE}
    DESCRIPTION "Real-time microphone processor with VST3 plugin chain"
    LANGUAGES C CXX)

# Re-configure when VERSION changes, which a plain file(READ) does not cause.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RV_ENABLE_VST3   "Build the VST3 plugin host"                    ON)
option(RV_ENABLE_RNNOISE "Build the neural noise suppressor (RNNoise)"  ON)
option(RV_FETCH_VST3    "Download the VST3 SDK sources automatically"   ON)

set(RV_VST3_SDK_DIR "" CACHE PATH
    "Root of a local VST3 SDK checkout (used when RV_FETCH_VST3=OFF)")
set(RV_ASIO_SDK_DIR "" CACHE PATH
    "Root of the Steinberg ASIO SDK (contains common/ and host/)")

# The ASIO SDK cannot be redistributed, so it is never fetched automatically and
# never lives in the repository. If it has been placed in the conventional spot
# - which tools/fetch-asio-sdk does - pick it up without the user having to say
# so twice. Nothing about ASIO changes for anyone who has not put it there.
set(_rv_bundled_asio ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk)
if(NOT RV_ASIO_SDK_DIR AND EXISTS ${_rv_bundled_asio}/common/asio.h)
    set(RV_ASIO_SDK_DIR ${_rv_bundled_asio} CACHE PATH "" FORCE)
    message(STATUS "ASIO SDK found at ${_rv_bundled_asio}")
endif()

if(EXISTS ${RV_ASIO_SDK_DIR}/common/asio.h)
    option(RV_ENABLE_ASIO "Build the ASIO backend (needs the ASIO SDK)" ON)
else()
    option(RV_ENABLE_ASIO "Build the ASIO backend (needs the ASIO SDK)" OFF)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "RadioVoice targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RV_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

# VST3-dependent translation units guard themselves with `#if RV_HAS_VST3`
# rather than being excluded here, so that the application still builds - just
# without plugin hosting - when the SDK is unavailable.
if(NOT RV_ENABLE_ASIO)
    list(FILTER RV_SOURCES EXCLUDE REGEX "Asio")
endif()

add_executable(RadioVoice WIN32 ${RV_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc)

# The .rc is left in the source tree rather than generated, so that its relative
# references to app.manifest and the icon keep resolving; only the version
# numbers it needs are generated, as a header it includes.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/res/rv_version.h.in
               ${CMAKE_BINARY_DIR}/generated/rv_version.h @ONLY)

target_include_directories(RadioVoice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/res    # so the .rc can find app.manifest
    ${CMAKE_BINARY_DIR}/generated)     # ... and the generated rv_version.h

target_compile_definitions(RadioVoice PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
    RV_VERSION="${PROJECT_VERSION}")

target_link_libraries(RadioVoice PRIVATE
    rv::imgui
    rv::json
    ole32 oleaut32 uuid propsys
    winmm avrt
    dsound dxguid
    d3d11 dxgi d3dcompiler
    shlwapi shell32 dwmapi comdlg32 user32 gdi32
    # The updater: WinHTTP fetches the release list and the installer, bcrypt
    # checks the SHA-256 GitHub publishes alongside it. Both ship with Windows.
    winhttp bcrypt)

if(TARGET rv::vst3sdk)
    target_link_libraries(RadioVoice PRIVATE rv::vst3sdk)
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_VST3=1)
else()
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_VST3=0)
endif()

if(TARGET rv::asio)
    target_link_libraries(RadioVoice PRIVATE rv::asio)
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_ASIO=1)
else()
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_ASIO=0)
endif()

if(TARGET rv::rnnoise)
    target_link_libraries(RadioVoice PRIVATE rv::rnnoise)
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_RNNOISE=1)
else()
    target_compile_definitions(RadioVoice PRIVATE RV_HAS_RNNOISE=0)
endif()

# ---------------------------------------------------------------------------
# Optimisation flags
#
# -ffast-math / /fp:fast are deliberately avoided: hosted plugins rely on
# well-defined NaN and infinity behaviour, and denormals are handled explicitly
# by setting FTZ/DAZ on the audio threads (see core/Denormal.h).
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(RadioVoice PRIVATE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /MP
        $<$<NOT:$<CONFIG:Debug>>:/O2 /Oi /Gy /fp:precise>)
    target_link_options(RadioVoice PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
else()
    target_compile_options(RadioVoice PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
        $<$<NOT:$<CONFIG:Debug>>:-O3 -ffp-contract=fast -fno-math-errno>)
    # -municode selects the wide-character CRT startup so that wWinMain is the
    # entry point; static runtime keeps the executable portable.
    #
    # -static, and not merely -static-libgcc -static-libstdc++. Those two leave
    # libwinpthread-1.dll behind as a runtime dependency, because MinGW's
    # libstdc++ is built against the POSIX threading model. On the machine that
    # compiled it that DLL is on PATH beside the compiler, so nothing looks
    # wrong; installed anywhere else the process dies before main with
    # 0xc0000142, which names neither the DLL nor the reason.
    target_link_options(RadioVoice PRIVATE
        -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)

    # A plain Release build is what gets handed to someone else, so the symbol
    # table goes too. Use the reldbg preset when a stack trace matters more
    # than the file size.
    target_link_options(RadioVoice PRIVATE $<$<CONFIG:Release>:-s>)
    target_compile_options(RadioVoice PRIVATE -ffunction-sections -fdata-sections)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "RadioVoice ${PROJECT_VERSION}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
if(TARGET rv::vst3sdk)
    message(STATUS "  VST3 host  : enabled")
else()
    message(STATUS "  VST3 host  : DISABLED")
endif()
if(TARGET rv::asio)
    message(STATUS "  ASIO       : enabled")
else()
    message(STATUS "  ASIO       : disabled (set RV_ENABLE_ASIO=ON and RV_ASIO_SDK_DIR)")
endif()
message(STATUS "")
