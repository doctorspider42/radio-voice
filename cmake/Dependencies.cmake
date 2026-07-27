# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Everything is fetched at configure time; nothing is vendored in the repo.
# Third-party CMake scripts are deliberately *not* executed (SOURCE_SUBDIR
# points at a non-existent directory) so that their own minimum-version
# requirements cannot clash with the CMake in use here. We build the handful
# of translation units we actually need ourselves.
# ---------------------------------------------------------------------------
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# Dear ImGui - MIT
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-is-not-used)
FetchContent_MakeAvailable(imgui)

add_library(rv_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(rv_imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(rv_imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

target_link_libraries(rv_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)
set_target_properties(rv_imgui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_library(rv::imgui ALIAS rv_imgui)

# ---------------------------------------------------------------------------
# nlohmann/json - MIT (single header, downloaded verbatim)
# ---------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json_header
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(nlohmann_json_header)

add_library(rv_json INTERFACE)
target_include_directories(rv_json SYSTEM INTERFACE ${nlohmann_json_header_SOURCE_DIR})
add_library(rv::json ALIAS rv_json)

# ---------------------------------------------------------------------------
# VST3 SDK - dual licensed GPLv3 / proprietary Steinberg licence.
#
# Only the three submodules required for *hosting* are checked out, and only
# the interface/base/hosting translation units are compiled. The plugin
# samples, the validator and VSTGUI are all skipped.
# ---------------------------------------------------------------------------
if(RV_ENABLE_VST3)
    if(RV_FETCH_VST3)
        message(STATUS "VST3 SDK: fetching from github.com/steinbergmedia/vst3sdk")
        FetchContent_Declare(vst3sdk
            GIT_REPOSITORY  https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG         master
            GIT_SHALLOW     TRUE
            GIT_SUBMODULES  "base;pluginterfaces;public.sdk"
            GIT_PROGRESS    TRUE
            SOURCE_SUBDIR   cmake-is-not-used)
        FetchContent_MakeAvailable(vst3sdk)
        set(_rv_vst3_root ${vst3sdk_SOURCE_DIR})
    else()
        if(NOT RV_VST3_SDK_DIR)
            message(FATAL_ERROR
                "RV_FETCH_VST3=OFF requires RV_VST3_SDK_DIR to point at a VST3 SDK checkout.")
        endif()
        set(_rv_vst3_root ${RV_VST3_SDK_DIR})
    endif()

    if(NOT EXISTS ${_rv_vst3_root}/pluginterfaces/base/funknown.h)
        message(WARNING
            "VST3 SDK not usable at '${_rv_vst3_root}' (pluginterfaces missing). "
            "The VST3 host will be disabled.")
    else()
        # Globbing rather than a fixed file list: the exact set of translation
        # units drifts between SDK releases.
        file(GLOB _rv_vst3_src
            ${_rv_vst3_root}/pluginterfaces/base/*.cpp
            ${_rv_vst3_root}/base/source/*.cpp
            ${_rv_vst3_root}/base/thread/source/*.cpp
            # The whole of source/common: commoniids.cpp is what defines every
            # Vst:: and gui interface IID, and commonstringconvert.cpp backs
            # the UTF-16 helpers the hosting code calls. Platform variants are
            # filtered out below.
            ${_rv_vst3_root}/public.sdk/source/common/*.cpp
            ${_rv_vst3_root}/public.sdk/source/vst/hosting/*.cpp
            ${_rv_vst3_root}/public.sdk/source/vst/utility/*.cpp
            # Defines Steinberg::Vst::<Interface>::iid for every VST interface.
            # The headers only declare them, so exactly one translation unit in
            # the process has to pull this in or nothing links.
            ${_rv_vst3_root}/public.sdk/source/vst/vstinitiids.cpp)

        # Drop other platforms, test harnesses and anything that drags in
        # the plugin-side (rather than host-side) of the SDK.
        #
        # `dataexchange` is also dropped: it pulls in alignedalloc.h, which
        # calls std::aligned_alloc - absent from libstdc++ on MinGW. It only
        # implements IDataExchangeHandler, an optional interface for bulk
        # plugin->host data transfer that an effect host does not need;
        # plugins asking for it simply get kNotImplemented.
        list(FILTER _rv_vst3_src EXCLUDE REGEX
            "(_mac|_linux|_ios)\\.(cpp|mm)$|/test|test\\.cpp$|main\\.cpp$|module_bundle|dataexchange")

        add_library(rv_vst3sdk STATIC ${_rv_vst3_src})

        target_include_directories(rv_vst3sdk SYSTEM PUBLIC ${_rv_vst3_root})

        target_compile_definitions(rv_vst3sdk PUBLIC
            $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00)

        target_link_libraries(rv_vst3sdk PUBLIC ole32 oleaut32 uuid shlwapi shell32)

        set_target_properties(rv_vst3sdk PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

        # The SDK is not warning-clean under -Wall/-W4; it is not our code.
        if(MSVC)
            target_compile_options(rv_vst3sdk PRIVATE /W0 /wd4005)
        else()
            target_compile_options(rv_vst3sdk PRIVATE -w)
        endif()

        add_library(rv::vst3sdk ALIAS rv_vst3sdk)

        message(STATUS "")
        message(STATUS "  *** VST3 SDK licence notice ***")
        message(STATUS "  The VST3 SDK is dual licensed: GPLv3, or a proprietary")
        message(STATUS "  Steinberg licence. Linking it makes this build GPLv3 unless")
        message(STATUS "  you hold a Steinberg agreement. See NOTICE.md.")
        message(STATUS "")
    endif()
endif()

# ---------------------------------------------------------------------------
# ASIO SDK - Steinberg licence, not redistributable, must be supplied locally.
# ---------------------------------------------------------------------------
if(RV_ENABLE_ASIO)
    if(NOT RV_ASIO_SDK_DIR OR NOT EXISTS ${RV_ASIO_SDK_DIR}/common/asio.h)
        message(WARNING
            "RV_ENABLE_ASIO=ON but RV_ASIO_SDK_DIR does not contain common/asio.h. "
            "ASIO support will be disabled. Download the SDK from "
            "https://www.steinberg.net/developers/ and point RV_ASIO_SDK_DIR at it.")
    else()
        add_library(rv_asio STATIC
            ${RV_ASIO_SDK_DIR}/common/asio.cpp
            ${RV_ASIO_SDK_DIR}/host/asiodrivers.cpp
            ${RV_ASIO_SDK_DIR}/host/pc/asiolist.cpp)
        target_include_directories(rv_asio SYSTEM PUBLIC
            ${RV_ASIO_SDK_DIR}/common
            ${RV_ASIO_SDK_DIR}/host
            ${RV_ASIO_SDK_DIR}/host/pc)
        target_link_libraries(rv_asio PUBLIC ole32 uuid)
        set_target_properties(rv_asio PROPERTIES CXX_STANDARD 20)
        if(MSVC)
            target_compile_options(rv_asio PRIVATE /W0)
        else()
            target_compile_options(rv_asio PRIVATE -w)
        endif()
        add_library(rv::asio ALIAS rv_asio)
    endif()
endif()
