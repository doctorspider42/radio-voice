#pragma once

#if RV_HAS_VST3

#include <windows.h>

#include <functional>
#include <string>

#include "base/source/fobject.h"
#include "pluginterfaces/gui/iplugview.h"

namespace rv::host {

/// Top-level Win32 window that hosts a plugin's own editor.
///
/// VST3 editors are given a parent HWND and draw into it themselves. A separate
/// top-level window per plugin - rather than docking the view inside the main
/// window - is what plugin UIs expect: many of them size themselves freely,
/// open their own popups and assume they own their host window.
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

    HWND hwnd_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    bool resizable_ = false;
};

} // namespace rv::host

#endif // RV_HAS_VST3
