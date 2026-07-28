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

# ---------------------------------------------------------------------------
# RNNoise - BSD-3-Clause (Xiph.Org). Verified against the COPYING file in the
# pinned tree, not from memory.
#
# The weights are not in the repository: upstream keeps them in a tarball named
# after its own SHA-256, which `model_version` holds. Reading that file and
# feeding it to EXPECTED_HASH means the download is verified against a value
# that travels with the pinned source, so the two can never drift apart.
# ---------------------------------------------------------------------------
if(RV_ENABLE_RNNOISE)
    FetchContent_Declare(rnnoise
        GIT_REPOSITORY https://github.com/xiph/rnnoise.git
        GIT_TAG        70f1d256acd4b34a572f999a05c87bf00b67730d
        SOURCE_SUBDIR  cmake-is-not-used)
    FetchContent_MakeAvailable(rnnoise)

    file(READ ${rnnoise_SOURCE_DIR}/model_version RV_RNNOISE_MODEL_HASH)
    string(STRIP "${RV_RNNOISE_MODEL_HASH}" RV_RNNOISE_MODEL_HASH)

    if(NOT EXISTS ${rnnoise_SOURCE_DIR}/src/rnnoise_data.c)
        set(RV_RNNOISE_MODEL_ARCHIVE
            ${CMAKE_BINARY_DIR}/rnnoise_data-${RV_RNNOISE_MODEL_HASH}.tar.gz)

        message(STATUS "Downloading the RNNoise model (about 56 MB)")
        file(DOWNLOAD
            https://media.xiph.org/rnnoise/models/rnnoise_data-${RV_RNNOISE_MODEL_HASH}.tar.gz
            ${RV_RNNOISE_MODEL_ARCHIVE}
            EXPECTED_HASH SHA256=${RV_RNNOISE_MODEL_HASH}
            SHOW_PROGRESS
            STATUS RV_RNNOISE_DOWNLOAD_STATUS)

        list(GET RV_RNNOISE_DOWNLOAD_STATUS 0 RV_RNNOISE_DOWNLOAD_CODE)
        if(NOT RV_RNNOISE_DOWNLOAD_CODE EQUAL 0)
            list(GET RV_RNNOISE_DOWNLOAD_STATUS 1 RV_RNNOISE_DOWNLOAD_MESSAGE)
            message(FATAL_ERROR
                "Could not download the RNNoise model: ${RV_RNNOISE_DOWNLOAD_MESSAGE}. "
                "Configure with -DRV_ENABLE_RNNOISE=OFF to build without it.")
        endif()

        # Only the two generated sources are wanted; the archive also carries
        # the PyTorch checkpoints they were produced from, which are of no use
        # at build time and account for nearly all of its size.
        file(ARCHIVE_EXTRACT INPUT ${RV_RNNOISE_MODEL_ARCHIVE}
             DESTINATION ${rnnoise_SOURCE_DIR}
             PATTERNS src/rnnoise_data.c src/rnnoise_data.h)
    endif()

    add_library(rv_rnnoise STATIC
        ${rnnoise_SOURCE_DIR}/src/denoise.c
        ${rnnoise_SOURCE_DIR}/src/rnn.c
        ${rnnoise_SOURCE_DIR}/src/pitch.c
        ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
        ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
        ${rnnoise_SOURCE_DIR}/src/nnet.c
        ${rnnoise_SOURCE_DIR}/src/nnet_default.c
        ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
        ${rnnoise_SOURCE_DIR}/src/rnnoise_data.c
        ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c)

    target_include_directories(rv_rnnoise SYSTEM PUBLIC
        ${rnnoise_SOURCE_DIR}/include
        ${rnnoise_SOURCE_DIR}/src)

    # Runtime dispatch, not a compile-time -march. The network is three GRUs of
    # 384 units evaluated every 10 ms, which is affordable on AVX2 and much less
    # so on the plain SSE2 fallback - but a binary built for AVX2 outright would
    # simply not start on a machine without it.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|AMD64|amd64|i.86" OR WIN32)
        target_sources(rv_rnnoise PRIVATE
            ${rnnoise_SOURCE_DIR}/src/x86/x86_dnn_map.c
            ${rnnoise_SOURCE_DIR}/src/x86/x86cpu.c
            ${rnnoise_SOURCE_DIR}/src/x86/nnet_sse4_1.c
            ${rnnoise_SOURCE_DIR}/src/x86/nnet_avx2.c)

        # CPU_INFO_BY_ASM is what upstream's own configure defines; the RTCD
        # code refuses to compile without being told how to reach CPUID, and
        # inline assembly is the only method it offers on GCC and Clang. MSVC
        # has the __cpuid intrinsic instead.
        target_compile_definitions(rv_rnnoise PRIVATE RNN_ENABLE_X86_RTCD)
        if(MSVC)
            target_compile_definitions(rv_rnnoise PRIVATE CPU_INFO_BY_C)
        else()
            target_compile_definitions(rv_rnnoise PRIVATE CPU_INFO_BY_ASM)
        endif()

        if(MSVC)
            set_source_files_properties(${rnnoise_SOURCE_DIR}/src/x86/nnet_avx2.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        else()
            set_source_files_properties(${rnnoise_SOURCE_DIR}/src/x86/nnet_sse4_1.c
                PROPERTIES COMPILE_OPTIONS "-msse4.1")
            set_source_files_properties(${rnnoise_SOURCE_DIR}/src/x86/nnet_avx2.c
                PROPERTIES COMPILE_OPTIONS "-mavx;-mfma;-mavx2")
        endif()
    endif()

    if(MSVC)
        target_compile_options(rv_rnnoise PRIVATE /W0)
    else()
        target_compile_options(rv_rnnoise PRIVATE -w)
    endif()

    add_library(rv::rnnoise ALIAS rv_rnnoise)
endif()
