// Platform layer: window, Direct3D 11 swap chain and the frame loop.
//
// Everything above this file is platform-agnostic in structure; this is the
// only place that knows about HWNDs and device contexts.

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <filesystem>
#include <memory>
#include <string>

#include "app/App.h"
#include "core/Autostart.h"
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

/// Message the shell sends for activity on the notification icon.
constexpr UINT WM_RV_TRAY = WM_APP + 1;

/// Identifies our icon within this window. Only one, so any value will do.
constexpr UINT kTrayIconId = 1;

/// A missing Explorer or notification area should not turn the one-second
/// status refresh into a stream of failed Shell_NotifyIcon calls.
constexpr ULONGLONG kTrayRetryDelayMs = 15'000;

enum TrayCommand : UINT {
    kTrayToggleWindow = 1,
    kTrayMute,
    kTrayStartStop,
    kTrayInstallUpdate,
    kTrayExit,
};

HINSTANCE g_instance = nullptr;

bool g_trayIconAdded = false;
/// The earliest time at which a failed icon registration is retried.
ULONGLONG g_nextTrayIconAttempt = 0;
/// One warning per uninterrupted failure is enough; a tray icon may be
/// unavailable briefly while Explorer is restarting.
bool g_trayIconFailureLogged = false;
/// Whether the balloon explaining where the window went has already been shown.
/// Once per run: it is an answer to a surprise, and it stops being one.
bool g_trayHintShown = false;

/// Set when the user chose Exit rather than closing the window, so that
/// WM_CLOSE knows not to hide the window instead of acting on it.
bool g_exitRequested = false;

/// Broadcast by the shell when Explorer restarts, taking every notification
/// icon with it. Registered rather than constant, so it cannot be a switch case.
UINT g_taskbarCreatedMessage = 0;

/// Broadcast by a second copy of RadioVoice to ask this one to show itself.
UINT g_showWindowMessage = 0;

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

/// Supplies a usable tray image even if the executable's icon resource cannot
/// be loaded, for example from a damaged or partially replaced installation.
HICON loadTrayIcon()
{
    HICON icon = loadAppIcon(g_instance, SM_CXSMICON, SM_CYSMICON);
    if (!icon)
        icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

// ---------------------------------------------------------------------------
// Notification area
//
// The window and the engine are deliberately independent: hiding one does not
// touch the other. A microphone processor has nothing to show once it is set
// up, and everything to lose by stopping.
// ---------------------------------------------------------------------------

NOTIFYICONDATAW trayIconBase(HWND hwnd)
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd   = hwnd;
    data.uID    = kTrayIconId;
    return data;
}

/// Copies the application's one-line status into the icon's tooltip.
void updateTrayTooltip(HWND hwnd)
{
    if (!g_trayIconAdded || !g_app)
        return;

    NOTIFYICONDATAW data = trayIconBase(hwnd);
    data.uFlags = NIF_TIP;

    const std::wstring text = rv::toWide(g_app->trayTooltip());
    ::lstrcpynW(data.szTip, text.c_str(), ARRAYSIZE(data.szTip));

    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void addTrayIcon(HWND hwnd)
{
    if (g_trayIconAdded)
        return;

    const ULONGLONG now = ::GetTickCount64();
    if (now < g_nextTrayIconAttempt)
        return;

    NOTIFYICONDATAW data = trayIconBase(hwnd);
    data.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_RV_TRAY;
    data.hIcon            = loadTrayIcon();
    ::lstrcpynW(data.szTip, kWindowTitle, ARRAYSIZE(data.szTip));

    if (!data.hIcon || !::Shell_NotifyIconW(NIM_ADD, &data)) {
        g_nextTrayIconAttempt = now + kTrayRetryDelayMs;
        if (!g_trayIconFailureLogged) {
            RV_WARN("could not add the notification icon; retrying in %llu seconds",
                    static_cast<unsigned long long>(kTrayRetryDelayMs / 1000));
            g_trayIconFailureLogged = true;
        }
        return;
    }

    g_trayIconAdded = true;
    g_nextTrayIconAttempt = 0;
    g_trayIconFailureLogged = false;

    // The most recent notification-area behaviour is opt-in after every
    // successful registration, including one after Explorer restarts.
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);

    updateTrayTooltip(hwnd);
}

void removeTrayIcon(HWND hwnd)
{
    if (!g_trayIconAdded)
        return;

    NOTIFYICONDATAW data = trayIconBase(hwnd);
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayIconAdded = false;
}

/// Says once, the first time the window disappears, where it went. A window
/// that vanishes with no explanation reads as a crash.
void showTrayHint(HWND hwnd)
{
    if (g_trayHintShown || !g_trayIconAdded)
        return;

    NOTIFYICONDATAW data = trayIconBase(hwnd);
    data.uFlags       = NIF_INFO;
    data.dwInfoFlags  = NIIF_INFO;
    ::lstrcpynW(data.szInfoTitle, L"RadioVoice is still running",
                ARRAYSIZE(data.szInfoTitle));
    ::lstrcpynW(data.szInfo,
                L"Your microphone is still being processed. "
                L"Click the icon to bring the window back.",
                ARRAYSIZE(data.szInfo));

    ::Shell_NotifyIconW(NIM_MODIFY, &data);
    g_trayHintShown = true;
}

/// Says that an update has finished downloading.
///
/// Shown only while the window is away, which is the case this exists for: the
/// interface has a lit button for it, and a machine where RadioVoice has been
/// hidden for a fortnight has nobody looking at that button.
void showUpdateBalloon(HWND hwnd, const std::string& version)
{
    if (!g_trayIconAdded)
        return;

    NOTIFYICONDATAW data = trayIconBase(hwnd);
    data.uFlags      = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;

    const std::wstring text =
        L"Version " + rv::toWide(version) +
        L" has been downloaded. Right-click this icon to install it.";

    ::lstrcpynW(data.szInfoTitle, L"A RadioVoice update is ready",
                ARRAYSIZE(data.szInfoTitle));
    ::lstrcpynW(data.szInfo, text.c_str(), ARRAYSIZE(data.szInfo));

    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void hideToTray(HWND hwnd)
{
    // Added before the window goes, not after: a window that disappears while
    // the icon fails to appear leaves nothing to click.
    addTrayIcon(hwnd);
    if (!g_trayIconAdded)
        return;

    ::ShowWindow(hwnd, SW_HIDE);

    // Nothing renders while the window is hidden, and rendering is what
    // normally flushes the configuration.
    if (g_app)
        g_app->saveConfigNow();

    showTrayHint(hwnd);
}

void restoreFromTray(HWND hwnd)
{
    ::ShowWindow(hwnd, SW_SHOW);
    if (::IsIconic(hwnd))
        ::ShowWindow(hwnd, SW_RESTORE);
    ::SetForegroundWindow(hwnd);
}

bool windowIsOnScreen(HWND hwnd)
{
    return ::IsWindowVisible(hwnd) && !::IsIconic(hwnd);
}

void toggleWindow(HWND hwnd)
{
    if (windowIsOnScreen(hwnd))
        hideToTray(hwnd);
    else
        restoreFromTray(hwnd);
}

void showTrayMenu(HWND hwnd)
{
    HMENU menu = ::CreatePopupMenu();
    if (!menu)
        return;

    const bool onScreen = windowIsOnScreen(hwnd);
    ::AppendMenuW(menu, MF_STRING, kTrayToggleWindow,
                  onScreen ? L"Hide window" : L"Open RadioVoice");
    ::SetMenuDefaultItem(menu, kTrayToggleWindow, FALSE);

    if (g_app) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING | (g_app->isMuted() ? MF_CHECKED : 0u), kTrayMute,
                      L"Mute");
        ::AppendMenuW(menu, MF_STRING, kTrayStartStop,
                      g_app->isEngineRunning() ? L"Stop processing" : L"Start processing");
    }

    if (g_app && g_app->updateReady()) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const std::wstring item =
            L"Install update " + rv::toWide(g_app->updateVersion()) + L"...";
        ::AppendMenuW(menu, MF_STRING, kTrayInstallUpdate, item.c_str());
    }

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kTrayExit, L"Exit");

    POINT cursor{};
    ::GetCursorPos(&cursor);

    // Both of these are required, and neither is obvious. Without the
    // foreground call the menu will not dismiss when the user clicks away from
    // it; without the posted message the next click on the icon is swallowed.
    // The owner window is usually hidden here, which is exactly the case the
    // workaround exists for.
    ::SetForegroundWindow(hwnd);
    const UINT chosen =
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                         cursor.x, cursor.y, 0, hwnd, nullptr);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    switch (chosen) {
        case kTrayToggleWindow:
            toggleWindow(hwnd);
            break;

        case kTrayMute:
            if (g_app) {
                g_app->setMuted(!g_app->isMuted());
                g_app->saveConfigNow();
                updateTrayTooltip(hwnd);
            }
            break;

        case kTrayStartStop:
            if (g_app) {
                g_app->toggleEngine();
                updateTrayTooltip(hwnd);
            }
            break;

        case kTrayInstallUpdate:
            // The same shutdown path as Exit. What differs is what the
            // application left behind for it: an installer to start once this
            // process is gone.
            if (g_app) {
                g_app->requestUpdateInstall();
                g_exitRequested = true;
                ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            break;

        case kTrayExit:
            // Routed through WM_CLOSE so that there is one shutdown path
            // rather than two. The flag is what stops that path from simply
            // hiding the window again.
            g_exitRequested = true;
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        default:
            break;
    }
}

LRESULT WINAPI windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam))
        return true;

    // Registered messages carry ids assigned at run time, so they cannot be
    // switch cases and have to be tested first.
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        // Explorer restarted and took every notification icon with it. The
        // window may well be hidden, in which case this icon is the only way
        // back to it.
        if (g_trayIconAdded || (g_app && g_app->trayEnabled())) {
            g_trayIconAdded = false;
            g_nextTrayIconAttempt = 0;
            addTrayIcon(hwnd);
        }
        return 0;
    }

    if (message == g_showWindowMessage && g_showWindowMessage != 0) {
        restoreFromTray(hwnd);
        return 0;
    }

    switch (message) {
        case WM_RV_TRAY:
            // Only the button release, not the double click as well: acting on
            // both makes a double click toggle the window three times.
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                    toggleWindow(hwnd);
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    showTrayMenu(hwnd);
                    return 0;
                default:
                    break;
            }
            return 0;

        case WM_CLOSE:
            if (!g_exitRequested && g_app && g_app->closeToTray()) {
                hideToTray(hwnd);
                return 0;
            }
            break;  // DefWindowProc destroys the window, as usual

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                if (g_app && g_app->minimizeToTray())
                    hideToTray(hwnd);
                return 0;
            }
            g_resizeWidth   = LOWORD(lParam);
            g_resizeHeight  = HIWORD(lParam);
            g_resizePending = true;
            if (g_app)
                g_app->setWindowSize(static_cast<int>(g_resizeWidth),
                                     static_cast<int>(g_resizeHeight));
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

/// Whether `commandLine` carries `name`, in either the `--x` or `/x` spelling.
bool hasSwitch(PWSTR commandLine, const wchar_t* name)
{
    if (!commandLine)
        return false;

    wchar_t longForm[64];
    wchar_t shortForm[64];
    ::wsprintfW(longForm, L"--%s", name);
    ::wsprintfW(shortForm, L"/%s", name);

    return ::wcsstr(commandLine, longForm) != nullptr ||
           ::wcsstr(commandLine, shortForm) != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    // Setting the autostart entry is not a launch: it writes one registry value
    // and leaves, so it is handled before the window, the engine or the
    // single-instance lock exist.
    //
    // It is a separate mode because of who runs it. The installer is elevated,
    // and HKEY_CURRENT_USER inside an elevated process is the hive of whoever
    // answered the UAC prompt - not necessarily the person signing in. Inno
    // Setup's `runasoriginaluser` flag runs this as the actual user, so the
    // entry lands where it belongs, and the application remains the only thing
    // that ever writes it.
    {
        const bool enable  = hasSwitch(commandLine, L"enable-autostart");
        const bool disable = hasSwitch(commandLine, L"disable-autostart");
        if (enable || disable) {
            rv::log::init();
            const bool ok = rv::autostart::setEnabled(enable);
            rv::log::shutdown();
            return ok ? 0 : 1;
        }
    }

    g_instance = instance;

    // Both have to exist before the window can be told about either of them.
    g_taskbarCreatedMessage = ::RegisterWindowMessageW(L"TaskbarCreated");
    g_showWindowMessage     = ::RegisterWindowMessageW(L"RadioVoiceShowWindow");

    // A second copy would open the same devices and contend with the first for
    // them. A tray application is unusually easy to start twice, precisely
    // because the copy already running is not on screen to say so - so the
    // second one asks the first to show itself and leaves.
    //
    // Local\ rather than Global\: two users signed in at once each get their
    // own RadioVoice, which is the right answer for a per-user microphone.
    HANDLE instanceLock = ::CreateMutexW(nullptr, TRUE, L"Local\\RadioVoiceSingleInstance");
    if (instanceLock && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::PostMessageW(HWND_BROADCAST, g_showWindowMessage, 0, 0);
        ::CloseHandle(instanceLock);
        return 0;
    }

    // What the autostart entry passes, so that signing in does not throw a
    // window at the user.
    const bool minimizedRequested = hasSwitch(commandLine, L"minimized");

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

    // Read before the configuration is handed over, because it is moved from.
    // Starting hidden is only honoured when there is an icon to find the
    // window by; without one it would be an application with no way in.
    const bool startHidden =
        saved.trayEnabled && (saved.startMinimized || minimizedRequested);

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

    if (startHidden) {
        // Never shown at all, rather than shown and then hidden: a window that
        // flashes up during sign-in is the thing --minimized exists to avoid.
        addTrayIcon(hwnd);
        RV_INFO("starting hidden in the notification area");
    } else {
        ::ShowWindow(hwnd, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd);
    }

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

        // The icon and its tooltip are maintained here rather than from the
        // frame, because the interesting case is the one where no frame is
        // drawn at all. A second is often enough for a tooltip nobody is
        // looking at, and cheap enough to do from the idle path.
        {
            static ULONGLONG lastTraySync = 0;
            const ULONGLONG now = ::GetTickCount64();
            if (now - lastTraySync >= 1000) {
                lastTraySync = now;

                if (g_app->trayEnabled())
                    addTrayIcon(hwnd);
                else if (windowIsOnScreen(hwnd))
                    removeTrayIcon(hwnd);  // never while it is the only way back

                updateTrayTooltip(hwnd);

                // Announced from here, and only with the window away: on screen
                // the version button says the same thing without interrupting
                // anyone. Left unclaimed while the window is up, so that hiding
                // it later still produces the balloon.
                if (!windowIsOnScreen(hwnd) && g_trayIconAdded) {
                    std::string version;
                    if (g_app->takeUpdateAnnouncement(version))
                        showUpdateBalloon(hwnd, version);
                }
            }
        }

        if (::IsIconic(hwnd) || !::IsWindowVisible(hwnd)) {
            // Nothing is visible; yield instead of spinning on the GPU. The
            // audio path is untouched by this - it does not run from here.
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

    // Read before the application goes: Setup cannot run while this process
    // holds the executable it is about to replace, so the launch happens at the
    // very end - after the engine, the window and the notification icon.
    const std::filesystem::path installer = app->pendingInstaller();

    app->shutdown();
    g_app = nullptr;
    app.reset();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    removeTrayIcon(hwnd);
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(kWindowClass, instance);

    // Released before Setup starts, not after: the installer relaunches
    // RadioVoice when it is done, and a lock this process was still holding
    // would turn that relaunch into a second copy asking the first to show
    // itself.
    if (instanceLock)
        ::CloseHandle(instanceLock);

    if (!installer.empty()) {
        // /SILENT rather than /VERYSILENT: the progress window is the only sign
        // the user gets that the thing they clicked is under way. The rest keeps
        // an unattended upgrade unattended - no message boxes, and no reboot
        // decided on their behalf.
        //
        // `relaunch` is ours. Inno Setup passes parameters it does not recognise
        // through to the script, where a [Run] entry starts RadioVoice again -
        // the wizard's own "Launch RadioVoice" checkbox does not appear in a
        // silent install, so without this the update would end with nothing
        // running.
        const std::wstring parameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /relaunch=yes";

        RV_INFO("starting the installer: %s", rv::toUtf8(installer.wstring()).c_str());

        // Setup's manifest asks for elevation, so this is where the user sees a
        // UAC prompt. There is no way round it: the application lives under
        // Program Files because the driver half of the installer has to.
        const HINSTANCE result =
            ::ShellExecuteW(nullptr, L"open", installer.c_str(), parameters.c_str(),
                            nullptr, SW_SHOWNORMAL);

        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            RV_ERROR("the installer could not be started (%lld)",
                     static_cast<long long>(reinterpret_cast<INT_PTR>(result)));
        }
    }

    RV_INFO("RadioVoice exiting");
    rv::log::shutdown();

    if (comOwned)
        ::CoUninitialize();

    return 0;
}
