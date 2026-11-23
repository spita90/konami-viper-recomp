/*
 * iOS services for the runtime (see platform_ios.h): the writable folder, the safe area, a display
 * link pacing the frame loop, tilt steering from CoreMotion and a haptic tap for the touch buttons.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreMotion/CoreMotion.h>
#include "platform_mobile.h"
#include "platform_ios.h"
#include <math.h>
#include <string.h>

const char *mobile_data_dir(void) {
    static char *dir;
    if (!dir) {
        NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *p = paths.count ? paths[0] : NSTemporaryDirectory();
        [[NSFileManager defaultManager] createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:NULL];
        dir = strdup(p.fileSystemRepresentation);
    }
    return dir;
}

static UIWindowScene *active_scene(void) {
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:[UIWindowScene class]]) return (UIWindowScene *)s;
    return nil;
}

static UIWindow *game_window(void) {
    UIWindowScene *scene = active_scene();
    for (UIWindow *w in scene.windows)
        if (w.isKeyWindow) return w;
    return scene.windows.firstObject;
}

void mobile_safe_area(float *left, float *top, float *right, float *bottom) {
    UIEdgeInsets e = game_window().safeAreaInsets;
    *left = (float)e.left;
    *top = (float)e.top;
    *right = (float)e.right;
    *bottom = (float)e.bottom;
}

/* ------------------------------------------------------------------ display link */
/* On the main run loop: the frame loop waits for the next refresh by running that run loop, so
 * UIKit delivers the touches (and its timers and animations go on) while it waits. A wait that
 * blocked the main thread left UIKit a few microseconds a frame (SDL's event pump), and SDL's
 * launch screen, faded out by an animation over the game's window, took the touches until the
 * app went to the background and back. */
@interface ViperVsync : NSObject
@property(nonatomic, strong) CADisplayLink *link;
@property(nonatomic) unsigned long long ticks;
@property(nonatomic) int fps;
@end

@implementation ViperVsync
- (void)tick:(CADisplayLink *)link {
    (void)link;
    self.ticks++;
}
- (void)applyRate {
    int fps = self.fps;
    if (@available(iOS 15.0, *)) {
        float f = fps > 0 ? (float)fps : 60.0f;
        self.link.preferredFrameRateRange = CAFrameRateRangeMake(f, f, f);
    } else {
        self.link.preferredFramesPerSecond = fps;
    }
}
@end

static ViperVsync *s_vsync;

void ios_vsync_start(int fps) {
    if (s_vsync) return;
    s_vsync = [ViperVsync new];
    s_vsync.fps = fps;
    s_vsync.link = [CADisplayLink displayLinkWithTarget:s_vsync selector:@selector(tick:)];
    [s_vsync applyRate];
    [s_vsync.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

void ios_vsync_set_rate(int fps) {
    if (!s_vsync || s_vsync.fps == fps) return;
    s_vsync.fps = fps;
    [s_vsync applyRate];
}

void ios_vsync_wait(void) {
    if (!s_vsync) return;
    unsigned long long t = s_vsync.ticks;
    CFAbsoluteTime limit = CFAbsoluteTimeGetCurrent() + 0.05;
    while (s_vsync.ticks == t) {
        CFTimeInterval left = limit - CFAbsoluteTimeGetCurrent();
        if (left <= 0) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, true);
    }
}

/* ------------------------------------------------------------------ tilt */
static CMMotionManager *s_motion;

int mobile_tilt_available(void) {
    if (!s_motion) s_motion = [CMMotionManager new];
    return s_motion.deviceMotionAvailable;
}

void mobile_tilt_enable(int on) {
    if (!mobile_tilt_available()) return;
    if (on && !s_motion.deviceMotionActive) {
        s_motion.deviceMotionUpdateInterval = 1.0 / 60.0;
        [s_motion startDeviceMotionUpdates];
    } else if (!on && s_motion.deviceMotionActive)
        [s_motion stopDeviceMotionUpdates];
}

/* Gravity is in the device's portrait frame (x right, y up). With the home side on the right
 * (UIInterfaceOrientationLandscapeRight) the screen's down is -x and its right -y; on the left,
 * +x and +y. Turning the device clockwise moves gravity towards the screen's right. */
int mobile_tilt(double *angle) {
    *angle = 0;
    CMDeviceMotion *m = s_motion.deviceMotion;
    if (!m) return 0;
    double gx = m.gravity.x, gy = m.gravity.y;
    UIInterfaceOrientation o = active_scene().interfaceOrientation;
    double down, right;
    if (o == UIInterfaceOrientationLandscapeLeft) { down = gx; right = gy; }
    else if (o == UIInterfaceOrientationLandscapeRight) { down = -gx; right = -gy; }
    else if (o == UIInterfaceOrientationPortraitUpsideDown) { down = gy; right = -gx; }
    else { down = -gy; right = gx; }
    if (fabs(down) + fabs(right) < 0.2) return 0;     /* lying flat: no steering from it */
    *angle = atan2(right, down);
    return 1;
}

/* ------------------------------------------------------------------ haptics */
void mobile_haptic_tap(void) {
    static UIImpactFeedbackGenerator *gen;
    if (!gen) gen = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [gen impactOccurred];
    [gen prepare];
}
