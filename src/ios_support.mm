// Apple platform helpers. iOS-specific bits are guarded; the file compiles as
// Obj-C++ on macOS too (some helpers have macOS variants), and is excluded from
// non-Apple builds by CMake.
#include <TargetConditionals.h>
#include <string>
#include <cstring>
#include <atomic>
#include <memory>
#include <unistd.h>
#include "thistle_gamepad.h"

extern "C" void thistle_ios_set_working_dir(void);
extern "C" void thistle_ios_init_audio_session(void);
extern "C" const char* thistle_apple_writable_dir(void);
extern "C" const char* thistle_apple_device_name(void);
extern "C" void thistle_apple_set_clipboard(const char* text);
extern "C" void thistle_apple_haptic(int style); // 0 light 1 medium 2 heavy 3 success
// Async HTTP (NSURLSession). Returns an opaque handle; poll it each frame.
extern "C" void* thistle_http_start(const char* method, const char* url, const char* body);
extern "C" int  thistle_http_poll(void* h, int* status, const char** body, int* len); // 1 = done
extern "C" void thistle_http_free(void* h);

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

// Gamepad polling works the same on macOS and iOS via GameController.framework.
void thistle_apple_poll_gamepad(ThistleGamepad* g) {
    std::memset(g, 0, sizeof(*g));
    @autoreleasepool {
        GCController* c = nil;
        for (GCController* cc in [GCController controllers]) { c = cc; break; }
        if (!c) return;
        GCExtendedGamepad* gp = c.extendedGamepad;
        if (!gp) return;
        g->connected = 1;

        NSString* vn = c.vendorName ? c.vendorName : @"Controller";
        std::strncpy(g->name, vn.UTF8String, sizeof(g->name) - 1);
        NSString* low = vn.lowercaseString;
        if ([low containsString:@"xbox"]) {
            g->kind = 1;
        } else if ([low containsString:@"dualsense"] || [low containsString:@"dualshock"] ||
                   [low containsString:@"sony"] || [low containsString:@"playstation"] ||
                   [low containsString:@"ps4"] || [low containsString:@"ps5"]) {
            g->kind = 2;
        } else if ([low containsString:@"nintendo"] || [low containsString:@"joy-con"] ||
                   [low containsString:@"joycon"] || [low containsString:@"switch"]) {
            g->kind = 3;
        } else {
            g->kind = 4;
        }
        g->a = gp.buttonA.isPressed;      g->b = gp.buttonB.isPressed;
        g->x = gp.buttonX.isPressed;      g->y = gp.buttonY.isPressed;
        g->up = gp.dpad.up.isPressed;     g->down = gp.dpad.down.isPressed;
        g->left = gp.dpad.left.isPressed; g->right = gp.dpad.right.isPressed;
        g->l1 = gp.leftShoulder.isPressed; g->r1 = gp.rightShoulder.isPressed;
        if (gp.buttonMenu) g->start = gp.buttonMenu.isPressed;
        if (gp.buttonOptions) g->back = gp.buttonOptions.isPressed;
        g->lx = gp.leftThumbstick.xAxis.value;  g->ly = gp.leftThumbstick.yAxis.value;
        g->rx = gp.rightThumbstick.xAxis.value; g->ry = gp.rightThumbstick.yAxis.value;
        g->lt = gp.leftTrigger.value;           g->rt = gp.rightTrigger.value;
    }
}

// --- async HTTP via NSURLSession (macOS + iOS) ------------------------------
// The state is a shared_ptr captured BOTH by the caller's handle and by the
// completion block, so freeing the handle mid-request can't cause a use-after-
// free — the in-flight block keeps the state alive until it fires.
namespace {
struct ThistleHttp {
    std::atomic<bool> done{false};
    int status = 0;
    std::string body;
};
using HttpPtr = std::shared_ptr<ThistleHttp>;
}
void* thistle_http_start(const char* method, const char* url, const char* body) {
    auto sp = std::make_shared<ThistleHttp>();
    @autoreleasepool {
        NSURL* u = url ? [NSURL URLWithString:[NSString stringWithUTF8String:url]] : nil;
        if (!u) { sp->done.store(true); return new HttpPtr(sp); }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
        req.HTTPMethod = [NSString stringWithUTF8String:(method && *method) ? method : "GET"];
        req.timeoutInterval = 15.0;
        if (body && *body) {
            [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
            req.HTTPBody = [NSData dataWithBytes:body length:std::strlen(body)];
        }
        HttpPtr cap = sp;   // captured by the block -> keeps state alive
        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
                if (!err) {
                    cap->status = (int)[(NSHTTPURLResponse*)resp statusCode];
                    if (data.length) cap->body.assign((const char*)data.bytes, data.length);
                }
                cap->done.store(true);
            }];
        [task resume];
    }
    return new HttpPtr(sp);
}
int thistle_http_poll(void* handle, int* status, const char** body, int* len) {
    auto* spp = static_cast<HttpPtr*>(handle);
    if (!spp || !(*spp) || !(*spp)->done.load()) return 0;
    ThistleHttp& h = **spp;
    if (status) *status = h.status;
    if (body) *body = h.body.c_str();
    if (len) *len = (int)h.body.size();
    return 1;
}
void thistle_http_free(void* handle) { delete static_cast<HttpPtr*>(handle); }

// Point the working directory at the app bundle's Resources so relative asset
// paths ("assets/foo.png") resolve when launched from Finder/Dock. Only does so
// when assets are actually bundled there, so running a non-bundled build from a
// project directory (the dev workflow) keeps working. Works on macOS and iOS.
void thistle_ios_set_working_dir(void) {
    @autoreleasepool {
        NSString* res = [[NSBundle mainBundle] resourcePath];
        if (!res) return;
        NSString* assets = [res stringByAppendingPathComponent:@"assets"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:assets]) {
            chdir([res fileSystemRepresentation]);
        }
    }
}

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

void thistle_ios_init_audio_session(void) {
    @autoreleasepool {
        AVAudioSession* s = [AVAudioSession sharedInstance];
        [s setCategory:AVAudioSessionCategoryAmbient error:nil];
        [s setActive:YES error:nil];
    }
}

const char* thistle_apple_writable_dir(void) {
    static std::string p;
    @autoreleasepool {
        NSArray* a = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        p = [[a firstObject] fileSystemRepresentation];
    }
    return p.c_str();
}

const char* thistle_apple_device_name(void) {
    static std::string p;
    @autoreleasepool { p = [[[UIDevice currentDevice] model] UTF8String]; }
    return p.c_str();
}

void thistle_apple_set_clipboard(const char* text) {
    @autoreleasepool {
        [UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:text];
    }
}

void thistle_apple_haptic(int style) {
    if (@available(iOS 10.0, *)) {
        @autoreleasepool {
            if (style == 3) {
                UINotificationFeedbackGenerator* g = [[UINotificationFeedbackGenerator alloc] init];
                [g notificationOccurred:UINotificationFeedbackTypeSuccess];
            } else {
                UIImpactFeedbackStyle s = UIImpactFeedbackStyleLight;
                if (style == 1) s = UIImpactFeedbackStyleMedium;
                else if (style == 2) s = UIImpactFeedbackStyleHeavy;
                UIImpactFeedbackGenerator* g = [[UIImpactFeedbackGenerator alloc] initWithStyle:s];
                [g impactOccurred];
            }
        }
    }
}

#else // macOS

void thistle_ios_init_audio_session(void) {}

const char* thistle_apple_writable_dir(void) {
    static std::string p;
    @autoreleasepool {
        NSArray* a = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        p = std::string([[a firstObject] fileSystemRepresentation]) + "/Thistle";
    }
    return p.c_str();
}

const char* thistle_apple_device_name(void) {
    static std::string p;
    @autoreleasepool { p = [[[NSHost currentHost] localizedName] UTF8String]; }
    return p.empty() ? "Mac" : p.c_str();
}

// On macOS the clipboard is handled by sokol_app (sapp_set_clipboard_string).
void thistle_apple_set_clipboard(const char* /*text*/) {}

void thistle_apple_haptic(int /*style*/) {} // no haptics on macOS

#endif

#else // non-Apple: no-op stubs (this file isn't compiled off Apple, but be safe)
void thistle_ios_set_working_dir(void) {}
void thistle_ios_init_audio_session(void) {}
const char* thistle_apple_writable_dir(void) { return ""; }
const char* thistle_apple_device_name(void) { return ""; }
void thistle_apple_set_clipboard(const char*) {}
void thistle_apple_haptic(int) {}
void* thistle_http_start(const char*, const char*, const char*) { return nullptr; }
int   thistle_http_poll(void*, int*, const char**, int*) { return 1; }
void  thistle_http_free(void*) {}
void thistle_apple_poll_gamepad(ThistleGamepad* g) { std::memset(g, 0, sizeof(*g)); }
#endif
