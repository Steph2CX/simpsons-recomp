// Plein ecran natif de macOS (son propre Space) : la barre de menus et la barre de titre avec le bouton vert
// apparaissent quand la souris arrive en haut de l'ecran.
#import <AppKit/AppKit.h>
#include <IOKit/IOKitLib.h>
#include <SDL3/SDL.h>

static NSWindow* CocoaWindow(SDL_Window* window)
{
    return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}

bool PlatformIsFullscreen(SDL_Window* window)
{
    NSWindow* cocoa = CocoaWindow(window);
    return cocoa != nil && ([cocoa styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

static void ShowTitleBar(NSWindow* cocoa, bool shown)
{
    [cocoa setTitlebarAppearsTransparent:!shown];
    [cocoa setTitleVisibility:shown ? NSWindowTitleVisible : NSWindowTitleHidden];
    [[cocoa standardWindowButton:NSWindowCloseButton] setHidden:!shown];
    [[cocoa standardWindowButton:NSWindowMiniaturizeButton] setHidden:!shown];
    [[cocoa standardWindowButton:NSWindowZoomButton] setHidden:!shown];
}

// The window (black) covers the whole screen with the Dock and menu bar hidden, otherwise the desktop shows around
// the window before and during the full screen animation. Back to normal when leaving full screen
void PlatformCoverScreen(SDL_Window* window)
{
    NSWindow* cocoa = CocoaWindow(window);
    if (cocoa == nil)
        return;
    static bool observing = false;
    if (!observing)
    {
        observing = true;
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidExitFullScreenNotification object:nil
            queue:nil usingBlock:^(NSNotification*) { [NSApp setPresentationOptions:NSApplicationPresentationDefault]; }];
        // Title bar hidden until then (it appears again in full screen when the mouse reaches the top)
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidEnterFullScreenNotification object:nil
            queue:nil usingBlock:^(NSNotification* notification) { ShowTitleBar([notification object], true); }];
    }
    ShowTitleBar(cocoa, false);
    // Auto-hide as in full screen (hidden made the Dock flash when the full screen Space took over)
    [NSApp setPresentationOptions:NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar];
    NSScreen* screen = [cocoa screen] ?: [NSScreen mainScreen];
    [cocoa setFrame:[screen frame] display:YES];
    // Above the Dock while it slides away (normal level again when full screen starts)
    [cocoa setLevel:NSStatusWindowLevel];
}

void PlatformToggleFullscreen(SDL_Window* window)
{
    NSWindow* cocoa = CocoaWindow(window);
    if (cocoa == nil)
        return;
    [cocoa setCollectionBehavior:[cocoa collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary];
    if (([cocoa styleMask] & NSWindowStyleMaskFullScreen) == 0)
        PlatformCoverScreen(window);
    [cocoa setLevel:NSNormalWindowLevel];
    [cocoa toggleFullScreen:nil];
}

int PlatformMenuCount()
{
    return [NSApp mainMenu] ? int([[NSApp mainMenu] numberOfItems]) : 0;
}

// Nombre de coeurs graphiques de la puce Apple (0 si inconnu) : 7 a 10 pour une puce de base (MacBook Air), 14 et plus
// pour les Pro/Max
int PlatformGpuCoreCount()
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (service == IO_OBJECT_NULL)
        return 0;
    int count = 0;
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
    if (value != nullptr)
    {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &count);
        CFRelease(value);
    }
    IOObjectRelease(service);
    return count;
}
