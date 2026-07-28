// Platform layer: window, Direct3D 11 swap chain and the frame loop.
//
// Everything above this file is platform-agnostic in structure; this is the
// only place that knows about HWNDs and device contexts.

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <memory>

#include "app/App.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "gui/Theme.h"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kWindowClass[] = L"RadioVoiceMain";
constexpr wchar_t kWindowTitle[] = L"RadioVoice";

ID3D11Device*           g_device        = nullptr;
ID3D11DeviceContext*    g_context       = nullptr;
IDXGISwapChain*         g_swapChain     = nullptr;
ID3D11RenderTargetView* g_renderTarget  = nullptr;

bool g_resizePending = false;
UINT g_resizeWidth   = 0;
UINT g_resizeHeight  = 0;

rv::app::App* g_app = nullptr;

/// Scale requested by a WM_DPICHANGED, or zero when there is none pending.
///
/// Acted on from the main loop rather than inside the message handler: the
/// response involves destroying the font texture, which cannot happen part-way
/// through a frame that is already using it.
float g_pendingDpiScale = 0.0f;

void createRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

void releaseRenderTarget()
{
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool createDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount       = 2;
    desc.BufferDesc.Width  = 0; // match the window
    desc.BufferDesc.Height = 0;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator   = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed   = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL       obtained = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc, &g_swapChain,
        &g_device, &obtained, &g_context);

    // A machine without a usable GPU driver still has WARP, and this
    // application's UI is light enough to run on it.
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        RV_WARN("no hardware Direct3D 11 device; falling back to the WARP renderer");
        hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &desc, &g_swapChain,
            &g_device, &obtained, &g_context);
    }

    if (FAILED(hr)) {
        RV_ERROR("could not create the Direct3D 11 device (0x%08lX)",
                 static_cast<unsigned long>(hr));
        return false;
    }

    createRenderTarget();
    return true;
}

void cleanupDeviceD3D()
{
    releaseRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

/// Asks DWM for a dark title bar so the frame matches the content.
void applyDarkTitleBar(HWND hwnd)
{
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows 10 and 11; 19 was
    // the value on Windows 10 builds before 2004. Trying both covers every
    // version that supports it, and the call is harmless where it does not.
    BOOL enabled = TRUE;
    if (FAILED(::DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled))))
        ::DwmSetWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
}

/// Loads the application icon at the system metric named by `widthMetric` and
/// `heightMetric`. Shared images, so they must not be destroyed.
HICON loadAppIcon(HINSTANCE instance, int widthMetric, int heightMetric)
{
    return static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON),
                                           IMAGE_ICON,
                                           ::GetSystemMetrics(widthMetric),
                                           ::GetSystemMetrics(heightMetric),
                                           LR_SHARED));
}

LRESULT WINAPI windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return true;

    switch (message) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeWidth   = LOWORD(lParam);
                g_resizeHeight  = HIWORD(lParam);
                g_resizePending = true;
                if (g_app)
                    g_app->setWindowSize(static_cast<int>(g_resizeWidth),
                                         static_cast<int>(g_resizeHeight));
            }
            return 0;

        case WM_DPICHANGED:
            // Windows supplies the rectangle the window should occupy on the
            // display it has arrived at. Honouring it is what keeps a window
            // dragged between a laptop panel and an external monitor the same
            // apparent size instead of doubling or halving.
            if (const RECT* suggested = reinterpret_cast<const RECT*>(lParam)) {
                ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                               suggested->right - suggested->left,
                               suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            g_pendingDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            return 0;

        case WM_SYSCOMMAND:
            // Swallow the Alt+Space system menu, which otherwise steals focus
            // while the user is typing into a field.
            if ((wParam & 0xFFF0) == SC_KEYMENU)
                return 0;
            break;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Apartment-threaded: plugin editors and the shell APIs used for the
    // device pickers expect an STA on the thread that owns the windows.
    const HRESULT comStatus = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comStatus);

    rv::log::init();
    RV_INFO("RadioVoice " RV_VERSION " starting");
    RV_INFO("configuration directory: %s",
            rv::toUtf8(rv::paths::dataDir().wstring()).c_str());

    auto app = std::make_unique<rv::app::App>();
    g_app = app.get();

    // The window has to be sized before the app exists, so the configuration is
    // read here and handed to the app rather than being parsed twice.
    rv::app::Config saved = rv::app::Config::load();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = instance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    // Asking for the exact metrics rather than passing LR_DEFAULTSIZE lets the
    // loader pick the matching image out of the icon group instead of scaling
    // the 32x32 one down for the title bar.
    wc.hIcon         = loadAppIcon(instance, SM_CXICON, SM_CYICON);
    wc.hIconSm       = loadAppIcon(instance, SM_CXSMICON, SM_CYSMICON);
    wc.hbrBackground = ::CreateSolidBrush(RGB(0x12, 0x14, 0x18));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  saved.windowWidth, saved.windowHeight,
                                  nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        RV_ERROR("could not create the main window");
        return 1;
    }

    applyDarkTitleBar(hwnd);

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        ::UnregisterClassW(kWindowClass, instance);
        ::MessageBoxW(nullptr,
                      L"Direct3D 11 could not be initialised. A graphics driver "
                      L"supporting feature level 10.0 or later is required.",
                      kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The layout is fixed and fully described by the code, so a stray ini file
    // would only ever restore a stale window arrangement.
    io.IniFilename = nullptr;

    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);

    // The saved size is in physical pixels, so a window sized on a 100% display
    // and reopened on a 200% one comes back at half its apparent size, holding
    // a layout whose text has doubled. Growing it to what the layout needs is
    // less surprising than a first run that opens cramped and overlapping.
    //
    // Only ever grows: a window the user deliberately made large is left alone.
    {
        const int minimumWidth  = static_cast<int>(1180.0f * dpiScale);
        const int minimumHeight = static_cast<int>(720.0f * dpiScale);

        RECT rect{};
        if (::GetWindowRect(hwnd, &rect)) {
            const int width  = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            if (width < minimumWidth || height < minimumHeight) {
                ::SetWindowPos(hwnd, nullptr, 0, 0,
                               (width < minimumWidth) ? minimumWidth : width,
                               (height < minimumHeight) ? minimumHeight : height,
                               SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            }
        }
    }

    rv::gui::applyTheme();
    ImGui::GetStyle().ScaleAllSizes(dpiScale);
    const rv::gui::Fonts fonts = rv::gui::loadFonts(dpiScale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    if (!app->initialize(hwnd, fonts, dpiScale, std::move(saved)))
        RV_ERROR("application initialisation reported a failure");

    const ImVec4 clearColour = rv::gui::theme::toVec4(rv::gui::theme::kBackground);

    bool running = true;
    while (running) {
        MSG message;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        if (::IsIconic(hwnd)) {
            // Nothing is visible; yield instead of spinning on the GPU.
            ::Sleep(10);
            continue;
        }

        if (g_pendingDpiScale > 0.0f) {
            const float scale = g_pendingDpiScale;
            g_pendingDpiScale = 0.0f;

            // The atlas holds glyphs rasterised for the old scale, so it is
            // rebuilt rather than resized - a scaled bitmap font is exactly the
            // blur this whole exercise exists to avoid.
            ImGui_ImplDX11_InvalidateDeviceObjects();
            ImGui::GetIO().Fonts->Clear();

            // The theme is reapplied before scaling because ScaleAllSizes
            // multiplies what is already there. Applied twice, the padding
            // would compound instead of tracking the display.
            rv::gui::applyTheme();
            ImGui::GetStyle().ScaleAllSizes(scale);

            const rv::gui::Fonts rescaled = rv::gui::loadFonts(scale);
            ImGui_ImplDX11_CreateDeviceObjects();

            if (g_app)
                g_app->setDpiScale(scale, rescaled);

            RV_INFO("display scale changed to %.0f%%", static_cast<double>(scale * 100.0f));
        }

        if (g_resizePending && g_resizeWidth > 0 && g_resizeHeight > 0) {
            releaseRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight,
                                       DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
            g_resizePending = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app->render();

        if (app->wantsExit())
            running = false;

        ImGui::Render();

        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        const float clear[4] = {clearColour.x, clearColour.y, clearColour.z, 1.0f};
        g_context->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Vsync: the UI has nothing to gain from running faster than the
        // display, and the audio threads are better off with the spare cycles.
        g_swapChain->Present(1, 0);
    }

    app->shutdown();
    g_app = nullptr;
    app.reset();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(kWindowClass, instance);

    RV_INFO("RadioVoice exiting");
    rv::log::shutdown();

    if (comOwned)
        ::CoUninitialize();

    return 0;
}
