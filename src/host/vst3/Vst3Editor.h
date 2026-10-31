#pragma once

#if RV_HAS_VST3

#include <windows.h>

#include <functional>
#include <string>

#include "base/source/fobject.h"
#include "pluginterfaces/gui/iplugview.h"

namespace rv::host {

/// Win32 window that hosts a plugin's own editor.
///
/// VST3 editors are given a parent HWND and draw into it themselves. The window
/// is either top-level - the editor popped out on its own, which is what plugin
/// UIs that size themselves freely are happiest with - or a child of the main
/// window, placed over the module view so the plugin sits in the same frame as
/// the built-in modules.
///
/// The class also implements `IPlugFrame`, which is how a plugin asks to be
/// resized (a resizable UI dragging its own corner, or switching to a different
/// skin size).
class Vst3EditorWindow : public Steinberg::FObject, public Steinberg::IPlugFrame {
public:
    Vst3EditorWindow();
    ~Vst3EditorWindow() override;

    /// Creates the window and attaches `view` to it. The view is addref'd for
    /// the lifetime of the window.
    bool open(Steinberg::IPlugView* view, const std::string& title, HWND owner);

    /// Creates a hidden child window of `parent` and attaches `view` to it.
    /// Place it with `place`; it stays hidden until then.
    bool openChild(Steinberg::IPlugView* view, HWND parent);

    /// Child windows only: moves the window's top-left corner to `x`, `y` in the
    /// parent's client coordinates and shows it.
    void place(int x, int y);

    /// Child windows only: takes it off screen without detaching the view.
    void hide();

    /// Child windows only: makes the editor as large as `areaWidth` x
    /// `areaHeight` allows. A resizable view is resized to the nearest size it
    /// accepts; a fixed one is scaled through IPlugViewContentScaleSupport when
    /// the plugin offers that, and otherwise keeps the size it has. Does
    /// nothing while the area stays the same, unless `force` is set.
    void fit(int areaWidth, int areaHeight, bool force = false);

    /// The size the plugin's view currently has, in pixels.
    bool viewSize(int& width, int& height) const;

    /// Detaches the view and destroys the window. Safe to call when closed.
    void close();

    bool isOpen() const { return hwnd_ != nullptr; }
    void raise();

    /// Invoked when the user closes the window. Runs on the UI thread.
    std::function<void()> onClosed;

    // --- IPlugFrame -------------------------------------------------------
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
                                             Steinberg::ViewRect* newSize) override;

    OBJ_METHODS(Vst3EditorWindow, Steinberg::FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(Steinberg::IPlugFrame)
    END_DEFINE_INTERFACES(Steinberg::FObject)
    REFCOUNT_METHODS(Steinberg::FObject)

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void registerWindowClass();

    /// Grows a client-area rectangle into the full window rectangle for the
    /// current style, so the plugin gets exactly the client size it asked for.
    void resizeToClient(int width, int height);

    /// Asks the view for the nearest size it accepts, scaling the request down
    /// and asking again while the answer is larger than the area.
    void shrinkToFit(Steinberg::ViewRect& rect, int areaWidth, int areaHeight);

    /// Shared tail of open and openChild: attaches the view to hwnd_.
    bool attach(Steinberg::IPlugView* view);

    HWND hwnd_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    bool resizable_ = false;
    bool child_     = false;

    /// What `fit` last worked towards, so an unchanged area costs nothing.
    int    fitWidth_   = 0;
    int    fitHeight_  = 0;
    /// A fixed-size editor's size at a content scale of 1, and the scale it is at.
    int    baseWidth_  = 0;
    int    baseHeight_ = 0;
    double scale_      = 1.0;
};

} // namespace rv::host

#endif // RV_HAS_VST3
