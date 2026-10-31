#include "host/vst3/Vst3Editor.h"

#if RV_HAS_VST3

#include <algorithm>
#include <cmath>

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

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

    if (!attach(view))
        return false;

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    return true;
}

bool Vst3EditorWindow::openChild(IPlugView* view, HWND parent)
{
    if (!view || !parent)
        return false;
    if (hwnd_)
        return true;

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

    // Embedded, there is no border to drag: the module view sizes the editor
    // itself through fit, as far as the plugin lets it.
    resizable_ = view->canResize() == kResultTrue;
    child_     = true;

    hwnd_ = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                              0, 0, rect.getWidth(), rect.getHeight(),
                              parent, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        RV_ERROR("could not create the embedded VST3 editor window");
        return false;
    }

    if (!attach(view))
        return false;

    viewSize(baseWidth_, baseHeight_);

    FUnknownPtr<IPlugViewContentScaleSupport> scaler(view_);
    RV_INFO("docked VST3 editor: %d x %d, %s, %s", baseWidth_, baseHeight_,
            resizable_ ? "resizable" : "fixed size",
            scaler ? "offers content scaling" : "no content scaling");
    return true;
}

bool Vst3EditorWindow::attach(IPlugView* view)
{
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

    return true;
}

void Vst3EditorWindow::place(int x, int y)
{
    if (!hwnd_ || !child_)
        return;

    ::SetWindowPos(hwnd_, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Vst3EditorWindow::hide()
{
    if (hwnd_ && child_ && ::IsWindowVisible(hwnd_))
        ::ShowWindow(hwnd_, SW_HIDE);
}

void Vst3EditorWindow::shrinkToFit(ViewRect& rect, int areaWidth, int areaHeight)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        view_->checkSizeConstraint(&rect);
        const int width  = rect.getWidth();
        const int height = rect.getHeight();
        if (width <= 0 || height <= 0 || (width <= areaWidth && height <= areaHeight))
            return;

        const double k = std::min(static_cast<double>(areaWidth) / width,
                                  static_cast<double>(areaHeight) / height);
        rect.right  = rect.left + static_cast<int32>(width * k);
        rect.bottom = rect.top + static_cast<int32>(height * k);
    }
    view_->checkSizeConstraint(&rect);
}

void Vst3EditorWindow::fit(int areaWidth, int areaHeight, bool force)
{
    if (!hwnd_ || !child_ || !view_ || areaWidth <= 0 || areaHeight <= 0)
        return;
    if (!force && areaWidth == fitWidth_ && areaHeight == fitHeight_)
        return;
    if (force)
        scale_ = 0.0;
    fitWidth_  = areaWidth;
    fitHeight_ = areaHeight;

    if (resizable_) {
        // Start from the editor's own proportions and let the plugin move the
        // rectangle to the nearest size it accepts - a minimum, a fixed
        // aspect, a step. A plugin that keeps its aspect from the width alone
        // can answer with something taller than the area, so the answer is
        // scaled down and asked about again until it fits.
        ViewRect rect{0, 0, areaWidth, areaHeight};
        if (baseWidth_ > 0 && baseHeight_ > 0) {
            const double k = std::min(static_cast<double>(areaWidth) / baseWidth_,
                                      static_cast<double>(areaHeight) / baseHeight_);
            rect.right  = static_cast<int32>(baseWidth_ * k);
            rect.bottom = static_cast<int32>(baseHeight_ * k);
        }
        shrinkToFit(rect, areaWidth, areaHeight);
        if (rect.getWidth() <= 0 || rect.getHeight() <= 0)
            return;
        resizeToClient(rect.getWidth(), rect.getHeight());
        view_->onSize(&rect);
        return;
    }

    // A fixed-size editor can still be zoomed, if it says it can. Plenty of
    // editors - JUCE's among them - redraw themselves at the factor and ask
    // for the matching size through resizeView.
    FUnknownPtr<IPlugViewContentScaleSupport> scaler(view_);
    if (!scaler || baseWidth_ <= 0 || baseHeight_ <= 0)
        return;

    const double factor = std::clamp(std::min(static_cast<double>(areaWidth) / baseWidth_,
                                              static_cast<double>(areaHeight) / baseHeight_),
                                     0.5, 4.0);
    if (std::abs(factor - scale_) < 0.01)
        return;
    if (scaler->setContentScaleFactor(static_cast<IPlugViewContentScaleSupport::ScaleFactor>(factor)) !=
        kResultTrue) {
        RV_INFO("VST3 editor declined a content scale of %.2f", factor);
        return;
    }
    scale_ = factor;

    // For an editor that rescales without asking to be resized.
    ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue && rect.getWidth() > 0 && rect.getHeight() > 0)
        resizeToClient(rect.getWidth(), rect.getHeight());
}

bool Vst3EditorWindow::viewSize(int& width, int& height) const
{
    if (!hwnd_)
        return false;

    RECT client{};
    if (!::GetClientRect(hwnd_, &client))
        return false;

    width  = client.right - client.left;
    height = client.bottom - client.top;
    return width > 0 && height > 0;
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

    ViewRect rect = *newSize;

    // Docked, the editor may not grow past the area it was given - a plugin's
    // own resize grip would otherwise drag it out of the window for good.
    // Shrinking is the user's call and is left alone.
    if (child_ && fitWidth_ > 0 && fitHeight_ > 0 &&
        (rect.getWidth() > fitWidth_ || rect.getHeight() > fitHeight_)) {
        shrinkToFit(rect, fitWidth_, fitHeight_);
    }

    resizeToClient(rect.getWidth(), rect.getHeight());

    // The view is told its new size only after the window actually has it,
    // which is the order the specification requires.
    return view->onSize(&rect);
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
            // A docked editor is sized by fit, which tells the view itself.
            if (self && self->view_ && self->resizable_ && !self->child_) {
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
