#include "host/vst3/Vst3Editor.h"

#if RV_HAS_VST3

#include "core/Log.h"
#include "core/Strings.h"

using namespace Steinberg;

namespace rv::host {
namespace {

constexpr wchar_t kWindowClass[] = L"RadioVoiceVst3Editor";
constexpr int     kFallbackWidth  = 480;
constexpr int     kFallbackHeight = 320;

bool g_classRegistered = false;

} // namespace

Vst3EditorWindow::Vst3EditorWindow() = default;

Vst3EditorWindow::~Vst3EditorWindow()
{
    close();
}

void Vst3EditorWindow::registerWindowClass()
{
    if (g_classRegistered)
        return;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Vst3EditorWindow::windowProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
    wc.lpszClassName = kWindowClass;

    ::RegisterClassExW(&wc);
    g_classRegistered = true;
}

bool Vst3EditorWindow::open(IPlugView* view, const std::string& title, HWND owner)
{
    if (!view)
        return false;

    if (hwnd_) {
        raise();
        return true;
    }

    // Refuse early rather than creating a window the plugin cannot use.
    if (view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        RV_ERROR("VST3 editor does not support HWND embedding");
        return false;
    }

    registerWindowClass();

    ViewRect rect{};
    if (view->getSize(&rect) != kResultTrue || rect.getWidth() <= 0 || rect.getHeight() <= 0) {
        rect.left = rect.top = 0;
        rect.right  = kFallbackWidth;
        rect.bottom = kFallbackHeight;
    }

    resizable_ = (view->canResize() == kResultTrue);

    // A non-resizable plugin window must not offer a resize border, otherwise
    // the user can drag it into a size the plugin will not honour.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        (resizable_ ? (WS_THICKFRAME | WS_MAXIMIZEBOX) : 0);

    RECT window{0, 0, rect.getWidth(), rect.getHeight()};
    ::AdjustWindowRectEx(&window, style, FALSE, 0);

    const std::wstring wideTitle = toWide(title);

    hwnd_ = ::CreateWindowExW(0, kWindowClass, wideTitle.c_str(), style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              window.right - window.left, window.bottom - window.top,
                              owner, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        RV_ERROR("could not create the VST3 editor window");
        return false;
    }

    view_ = view;
    view_->setFrame(this);

    if (view_->attached(hwnd_, kPlatformTypeHWND) != kResultTrue) {
        RV_ERROR("the VST3 editor refused to attach to its window");
        view_->setFrame(nullptr);
        view_.reset();
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // Some plugins only report their real size once attached.
    ViewRect attachedRect{};
    if (view_->getSize(&attachedRect) == kResultTrue &&
        attachedRect.getWidth() > 0 && attachedRect.getHeight() > 0) {
        resizeToClient(attachedRect.getWidth(), attachedRect.getHeight());
    }

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    return true;
}

void Vst3EditorWindow::close()
{
    if (!hwnd_)
        return;

    HWND hwnd = hwnd_;
    hwnd_ = nullptr; // stops the window procedure re-entering this path

    if (view_) {
        view_->removed();
        view_->setFrame(nullptr);
        view_.reset();
    }

    ::DestroyWindow(hwnd);
}

void Vst3EditorWindow::raise()
{
    if (!hwnd_)
        return;

    if (::IsIconic(hwnd_))
        ::ShowWindow(hwnd_, SW_RESTORE);
    ::SetForegroundWindow(hwnd_);
}

void Vst3EditorWindow::resizeToClient(int width, int height)
{
    if (!hwnd_)
        return;

    const DWORD style   = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));

    RECT rect{0, 0, width, height};
    ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    ::SetWindowPos(hwnd_, nullptr, 0, 0,
                   rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

tresult PLUGIN_API Vst3EditorWindow::resizeView(IPlugView* view, ViewRect* newSize)
{
    if (!view || !newSize || !hwnd_)
        return kInvalidArgument;

    resizeToClient(newSize->getWidth(), newSize->getHeight());

    // The view is told its new size only after the window actually has it,
    // which is the order the specification requires.
    return view->onSize(newSize);
}

LRESULT CALLBACK Vst3EditorWindow::windowProc(HWND hwnd, UINT message,
                                              WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<Vst3EditorWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }

        case WM_SIZE:
            if (self && self->view_ && self->resizable_) {
                RECT client{};
                ::GetClientRect(hwnd, &client);
                ViewRect rect{0, 0, client.right, client.bottom};
                // Only forward sizes the plugin agrees to; otherwise a drag
                // would leave the view and the window disagreeing.
                if (self->view_->checkSizeConstraint(&rect) == kResultTrue)
                    self->view_->onSize(&rect);
            }
            break;

        case WM_CLOSE:
            if (self) {
                // The callback destroys this object (the plugin drops its
                // editor), so it is copied to a local first: invoking a
                // std::function that frees the object holding it would be
                // executing from storage that has already been released.
                auto callback = self->onClosed;
                self->close();
                if (callback)
                    callback();
            }
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace rv::host

#endif // RV_HAS_VST3
