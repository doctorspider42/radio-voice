#include "host/vst3/Vst3Editor.h"
#if RV_HAS_VST3
#import <Cocoa/Cocoa.h>
#include "core/Log.h"
#include <algorithm>

@interface RVPluginWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) rv::host::Vst3EditorWindow* editor;
@end
@implementation RVPluginWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    // Keep the host container alive until the plugin is detached by close().
    [sender orderOut:nil];
    const auto callback = self.editor ? self.editor->onClosed : std::function<void()>{};
    if (callback) callback();
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification
{
    NSWindow* window = notification.object;
    if (self.editor) self.editor->nativeResized(window.contentView.bounds.size.width, window.contentView.bounds.size.height);
}
@end
namespace rv::host {
using namespace Steinberg;
Vst3EditorWindow::Vst3EditorWindow() = default;
Vst3EditorWindow::~Vst3EditorWindow() { close(); }
bool Vst3EditorWindow::attach(IPlugView* view)
{
    if (view->isPlatformTypeSupported(kPlatformTypeNSView) != kResultTrue) return false;
    view_ = view;
    resizable_ = view->canResize() == kResultTrue;
    view_->setFrame(this);
    if (view_->attached(hwnd_, kPlatformTypeNSView) != kResultTrue) {
        view_->setFrame(nullptr); view_ = nullptr; return false;
    }
    ViewRect size;
    if (view_->getSize(&size) == kResultTrue) {
        baseWidth_ = size.getWidth(); baseHeight_ = size.getHeight();
        resizeToClient(baseWidth_, baseHeight_);
    }
    return true;
}
bool Vst3EditorWindow::open(IPlugView* view, const std::string& title, NativeEditorParent)
{
    if (hwnd_) { raise(); return true; }
    auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (view->canResize() == kResultTrue) style |= NSWindowStyleMaskResizable;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
        styleMask:style backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.title = [NSString stringWithUTF8String:title.c_str()];
    NSView* container = [[NSView alloc] initWithFrame:window.contentView.bounds];
    window.contentView = container;
    hwnd_ = container;
    [container release];
    window_ = window;
    auto* delegate = [[RVPluginWindowDelegate alloc] init];
    delegate.editor = this;
    window.delegate = delegate;
    delegate_ = delegate;
    child_ = false;
    if (!attach(view)) { close(); return false; }
    [window center]; [window makeKeyAndOrderFront:nil];
    return true;
}
bool Vst3EditorWindow::openChild(IPlugView* view, NativeEditorParent parent)
{
    close();
    if (!parent) return false;
    NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 320)];
    [static_cast<NSView*>(parent) addSubview:container];
    container.hidden = YES;
    hwnd_ = container;
    child_ = true;
    if (!attach(view)) { close(); return false; }
    return true;
}
void Vst3EditorWindow::close()
{
    if (view_) { view_->setFrame(nullptr); view_->removed(); view_ = nullptr; }
    if (child_ && hwnd_) {
        auto* container = static_cast<NSView*>(hwnd_);
        [container removeFromSuperview]; [container release];
    }
    if (window_) {
        NSWindow* window = static_cast<NSWindow*>(window_);
        window.delegate = nil; [window orderOut:nil]; [window close]; [window release];
    }
    if (delegate_) [static_cast<RVPluginWindowDelegate*>(delegate_) release];
    hwnd_ = window_ = delegate_ = nullptr;
    child_ = false;
    fitWidth_ = fitHeight_ = baseWidth_ = baseHeight_ = 0;
}
void Vst3EditorWindow::nativeResized(int width, int height)
{
    if (view_ && !child_) {
        ViewRect rect(0, 0, width, height);
        view_->checkSizeConstraint(&rect);
        view_->onSize(&rect);
    }
}
void Vst3EditorWindow::place(int x, int y)
{
    if (!hwnd_ || !child_) return;
    auto* container = static_cast<NSView*>(hwnd_);
    [container setFrameOrigin:NSMakePoint(x, y)];
    container.hidden = NO;
}
void Vst3EditorWindow::hide() { if (child_ && hwnd_) [static_cast<NSView*>(hwnd_) setHidden:YES]; }
void Vst3EditorWindow::raise() { if (window_) [static_cast<NSWindow*>(window_) makeKeyAndOrderFront:nil]; }
bool Vst3EditorWindow::viewSize(int& width, int& height) const
{
    ViewRect size;
    if (!view_ || view_->getSize(&size) != kResultTrue) return false;
    width = size.getWidth(); height = size.getHeight(); return true;
}
void Vst3EditorWindow::resizeToClient(int width, int height)
{
    if (!hwnd_ || width <= 0 || height <= 0) return;
    if (window_) [static_cast<NSWindow*>(window_) setContentSize:NSMakeSize(width, height)];
    else [static_cast<NSView*>(hwnd_) setFrameSize:NSMakeSize(width, height)];
}
void Vst3EditorWindow::shrinkToFit(ViewRect& rect, int width, int height)
{
    view_->checkSizeConstraint(&rect);
    for (int i = 0; i < 12 && (rect.getWidth() > width || rect.getHeight() > height); ++i) {
        rect.right = std::max(1, static_cast<int>(rect.getWidth() * 0.9));
        rect.bottom = std::max(1, static_cast<int>(rect.getHeight() * 0.9));
        view_->checkSizeConstraint(&rect);
    }
}
void Vst3EditorWindow::fit(int width, int height, bool force)
{
    if (!child_ || !view_ || width <= 0 || height <= 0) return;
    if (!force && fitWidth_ == width && fitHeight_ == height) return;
    fitWidth_ = width; fitHeight_ = height;
    if (resizable_) {
        ViewRect rect(0, 0, width, height);
        shrinkToFit(rect, width, height);
        if (rect.getWidth() <= width && rect.getHeight() <= height) {
            resizeToClient(rect.getWidth(), rect.getHeight()); view_->onSize(&rect);
        }
    }
}
tresult PLUGIN_API Vst3EditorWindow::resizeView(IPlugView* view, ViewRect* rect)
{
    if (view != view_ || !rect) return kInvalidArgument;
    if (child_ && fitWidth_ > 0 && (rect->getWidth() > fitWidth_ || rect->getHeight() > fitHeight_)) return kResultFalse;
    resizeToClient(rect->getWidth(), rect->getHeight());
    return view_->onSize(rect);
}
}
#endif
