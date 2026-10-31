#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <AVFoundation/AVFoundation.h>
#include <imgui.h>
#include <imgui_impl_osx.h>
#include <imgui_impl_metal.h>
#include "app/App.h"
#include "core/Log.h"
#include "platform/macos/RouteProbe.h"
#include "gui/Theme.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>

@interface RVMetalView : MTKView
@end
@implementation RVMetalView
- (BOOL)isFlipped { return YES; }
@end

@interface RVApplication : NSObject <NSApplicationDelegate, NSWindowDelegate, MTKViewDelegate> {
    std::unique_ptr<rv::app::App> app_;
    rv::app::Config loadedConfig_;
    std::unique_ptr<rv::audio::RouteProbe> routeProbe_;
    NSWindow* window_;
    RVMetalView* view_;
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    NSStatusItem* status_;
    NSTimer* timer_;
    BOOL shuttingDown_;
    BOOL initialized_;
    BOOL minimized_;
    BOOL smoke_;
    BOOL resumeAfterPermission_;
    int frames_;
    int smokeFrames_;
    std::atomic<int> presentedFrames_;
    std::atomic<double> firstPresentation_;
    std::atomic<double> lastPresentation_;
    std::chrono::steady_clock::time_point smokeStarted_;
}
- (void)tick:(NSTimer*)timer;
- (void)show:(id)sender;
- (void)mute:(id)sender;
- (void)engine:(id)sender;
- (void)quit:(id)sender;
- (void)initializeProcessor;
- (void)startRouteVerification;
- (void)renderFrame:(MTKView*)view onscreen:(BOOL)onscreen;
@end

@implementation RVApplication
- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    rv::log::init();
    RV_INFO("RadioVoice %s starting on macOS", RV_VERSION);
    smoke_ = [[NSProcessInfo processInfo].arguments containsObject:@"--smoke-test"];
    presentedFrames_.store(0);
    firstPresentation_.store(0);
    lastPresentation_.store(0);
    smokeFrames_ = 120;
    smokeStarted_ = std::chrono::steady_clock::now();
    if (const char* value = std::getenv("RADIOVOICE_SMOKE_FRAMES")) smokeFrames_ = std::max(120, std::atoi(value));
    loadedConfig_ = rv::app::Config::load();
    const auto& config = loadedConfig_;
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        alert.messageText = @"RadioVoice needs a Metal-capable GPU.";
        [alert runModal]; [NSApp terminate:nil]; return;
    }
    queue_ = [device_ newCommandQueue];
    window_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, config.windowWidth, config.windowHeight)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    window_.title = @"RadioVoice";
    window_.minSize = NSMakeSize(980, 680);
    window_.releasedWhenClosed = NO;
    window_.delegate = self;
    window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    view_ = [[RVMetalView alloc] initWithFrame:window_.contentView.bounds device:device_];
    view_.delegate = self;
    view_.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view_.paused = YES;
    view_.enableSetNeedsDisplay = NO;
    window_.contentView = view_;
    [window_ center];

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    rv::gui::applyTheme();
    ImGui_ImplOSX_Init(view_);
    ImGui_ImplMetal_Init(device_);
    initialized_ = YES;

    app_ = std::make_unique<rv::app::App>();
    // App initialization and auto-start wait for macOS microphone permission.
    // The window still opens when permission was denied so the user can fix it.
    minimized_ = config.trayEnabled && (config.startMinimized || [[NSProcessInfo processInfo].arguments containsObject:@"--minimized"]);
    if (!minimized_) { [window_ makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
    const auto permission = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    resumeAfterPermission_ = config.autoStart || [[NSProcessInfo processInfo].arguments containsObject:@"--verify-route"];
    // Draw the interface while the OS permission dialog is pending. Permission
    // is needed for capture, not for browsing the rack or choosing devices.
    [self initializeProcessor];
    if (!smoke_ && permission == AVAuthorizationStatusNotDetermined) {
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (shuttingDown_ || !app_) return;
                if (granted) {
                    RV_INFO("Microphone access granted");
                    if (resumeAfterPermission_ && !app_->isEngineRunning()) app_->toggleEngine();
                    [self startRouteVerification];
                } else RV_WARN("Microphone access was denied. Enable RadioVoice in System Settings > Privacy & Security > Microphone.");
            });
        }];
    }

}
- (void)initializeProcessor
{
    if (shuttingDown_) return;
    auto config = std::move(loadedConfig_);
    const auto permission = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (smoke_ || permission != AVAuthorizationStatusAuthorized) {
        if (!smoke_) RV_WARN("Microphone access is disabled. Enable RadioVoice in System Settings > Privacy & Security > Microphone, then restart the audio engine.");
    }
    app_->initialize(view_, rv::gui::loadFonts(1.0f), 1.0f, std::move(config),
        !smoke_ && permission == AVAuthorizationStatusAuthorized);
    if (permission == AVAuthorizationStatusAuthorized) [self startRouteVerification];
    NSMenu* mainMenu = [[[NSMenu alloc] init] autorelease];
    NSMenuItem* root = [[[NSMenuItem alloc] init] autorelease];
    NSMenu* appMenu = [[[NSMenu alloc] initWithTitle:@"RadioVoice"] autorelease];
    NSMenuItem* show = [appMenu addItemWithTitle:@"Show RadioVoice" action:@selector(show:) keyEquivalent:@"0"];
    show.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quit = [appMenu addItemWithTitle:@"Quit RadioVoice" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = self;
    root.submenu = appMenu;
    [mainMenu addItem:root];
    NSApp.mainMenu = mainMenu;
    timer_ = [[NSTimer timerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES] retain];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
}
- (void)startRouteVerification
{
    if (![[NSProcessInfo processInfo].arguments containsObject:@"--verify-route"] || smoke_) return;
    if (!app_->isEngineRunning()) app_->toggleEngine();
    routeProbe_ = std::make_unique<rv::audio::RouteProbe>(*app_);
    routeProbe_->start();
}
- (void)tick:(NSTimer*)timer
{
    if (shuttingDown_ || !app_) return;
    @autoreleasepool {
        if (app_->trayEnabled() && !status_) {
            status_ = [[[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] retain];
            status_.button.title = @"RV";
            NSMenu* menu = [[[NSMenu alloc] initWithTitle:@"RadioVoice"] autorelease];
            for (NSArray* entry in @[@[@"Show RadioVoice", @"show:"], @[@"Mute microphone", @"mute:"],
                                     @[@"Start / stop processing", @"engine:"], @[@"Quit", @"quit:"]]) {
                NSMenuItem* item = [menu addItemWithTitle:entry[0] action:NSSelectorFromString(entry[1]) keyEquivalent:@""];
                item.target = self;
            }
            status_.menu = menu;
        } else if (!app_->trayEnabled() && status_) {
            [[NSStatusBar systemStatusBar] removeStatusItem:status_]; [status_ release]; status_ = nil;
            if (!window_.visible) [self show:nil];
        }
        if (status_) status_.button.toolTip = [NSString stringWithUTF8String:app_->trayTooltip().c_str()];
        // MTKView.draw recycles the drawable after the delegate returns.
        // Invoking the delegate directly repeatedly presents the same drawable.
        if (window_.visible && !window_.miniaturized) [view_ draw];
        else [self renderFrame:view_ onscreen:NO];
        if (routeProbe_) routeProbe_->tick();
        if (smoke_) {
            ++frames_;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - smokeStarted_).count();
            if (frames_ >= smokeFrames_ || elapsed > smokeFrames_ / 60.0 + 3.0) {
                const int presented = presentedFrames_.load();
                const double duration = lastPresentation_.load() - firstPresentation_.load();
                const double fps = duration > 0 ? (presented - 1) / duration : 0;
                const bool passed = presented >= smokeFrames_ * 3 / 4 && fps >= 30;
                std::printf("macOS stationary-window presentation test %s: %d presented / %d ticks, %.1f FPS\n",
                    passed ? "passed" : "FAILED", presented, frames_, fps);
                [self applicationWillTerminate:[NSNotification notificationWithName:NSApplicationWillTerminateNotification object:NSApp]];
                std::exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
            }
        }
        if (app_->wantsExit()) [NSApp terminate:nil];
    }
}
- (void)drawInMTKView:(MTKView*)view
{
    [self renderFrame:view onscreen:YES];
}
- (void)renderFrame:(MTKView*)view onscreen:(BOOL)onscreen
{
    if (!initialized_ || !app_ || shuttingDown_) return;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(view.bounds.size.width, view.bounds.size.height);
    const float scale = window_.backingScaleFactor;
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    MTLRenderPassDescriptor* pass = onscreen ? view.currentRenderPassDescriptor : nil;
    if (!pass) {
        // Keep config saving, device recovery and plugin parameter delivery alive
        // while hidden. No GPU command buffer or drawable is needed for that.
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
    }
    ImGui_ImplMetal_NewFrame(pass);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();
    app_->setWindowSize(static_cast<int>(view.bounds.size.width), static_cast<int>(view.bounds.size.height));
    app_->render();
    ImGui::Render();
    if (!onscreen || !pass.colorAttachments[0].texture) return;
    id<MTLCommandBuffer> command = [queue_ commandBuffer];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.05, 0.06, 1);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command, encoder);
    [encoder endEncoding];
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (smoke_) {
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            const double time = shown.presentedTime;
            double empty = 0;
            firstPresentation_.compare_exchange_strong(empty, time);
            lastPresentation_.store(time);
            presentedFrames_.fetch_add(1);
        }];
    }
    [command presentDrawable:drawable];
    [command commit];
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}
- (void)show:(id)sender { [window_ deminiaturize:nil]; [window_ makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
- (void)mute:(id)sender { app_->setMuted(!app_->isMuted()); app_->saveConfigNow(); }
- (void)engine:(id)sender { app_->toggleEngine(); app_->saveConfigNow(); }
- (void)quit:(id)sender { [NSApp terminate:nil]; }
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    if (app_ && app_->closeToTray()) { [sender orderOut:nil]; return NO; }
    [NSApp terminate:nil]; return NO;
}
- (void)windowDidMiniaturize:(NSNotification*)note
{
    if (app_ && app_->minimizeToTray()) [window_ orderOut:nil];
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible
{
    [self show:nil]; return YES;
}
- (void)applicationWillTerminate:(NSNotification*)notification
{
    shuttingDown_ = YES;
    [timer_ invalidate]; [timer_ release]; timer_ = nil;
    routeProbe_.reset();
    if (app_) { app_->shutdown(); app_.reset(); }
    if (initialized_) {
        ImGui_ImplMetal_Shutdown(); ImGui_ImplOSX_Shutdown(); ImGui::DestroyContext(); initialized_ = NO;
    }
    if (status_) { [[NSStatusBar systemStatusBar] removeStatusItem:status_]; [status_ release]; status_ = nil; }
    rv::log::shutdown();
}
@end

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc > 1 && std::string(argv[1]) == "--list-devices") {
            rv::audio::DeviceEnumerator devices;
            devices.refresh();
            for (const auto& d : devices.all())
                std::printf("%s\t%s\t%d ch\t%d Hz\t%s%s\n", d.isInput ? "input" : "output",
                    d.name.c_str(), d.isInput ? d.maxInputChannels : d.maxOutputChannels,
                    d.defaultSampleRate, d.id.c_str(), d.isVirtualCable ? "\tvirtual" : "");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--smoke-test") {
            char directory[] = "/tmp/radiovoice-smoke-XXXXXX";
            if (!mkdtemp(directory)) return 1;
            setenv("RADIOVOICE_DATA_DIR", directory, 1);
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        RVApplication* delegate = [[RVApplication alloc] init];
        NSApp.delegate = delegate;
        [NSApp run];
        [delegate release];
    }
    return 0;
}
