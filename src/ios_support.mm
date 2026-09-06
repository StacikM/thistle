// Apple platform helpers. iOS-specific bits are guarded; the file compiles as
// Obj-C++ on macOS too (some helpers have macOS variants), and is excluded from
// non-Apple builds by CMake.
#include <TargetConditionals.h>
#include <string>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
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
// StoreKit (in-app purchases). Products/events cross as JSON strings — see
// the matching declarations + doc comments in thistle.cpp/thistle.hpp.
extern "C" int         thistle_iap_can_make_payments(void);
extern "C" void        thistle_iap_fetch_products(const char* ids_json);
extern "C" int         thistle_iap_products_ready(void);
extern "C" const char* thistle_iap_products_json(void);
extern "C" void        thistle_iap_purchase(const char* product_id);
extern "C" void        thistle_iap_restore_purchases(void);
extern "C" void        thistle_iap_finish_transaction(const char* transaction_id);
extern "C" const char* thistle_iap_poll_event_json(void);

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

// --- in-app purchases via StoreKit (macOS + iOS) ----------------------------
// StoreKit 1 (the classic Objective-C API), not StoreKit 2 — StoreKit 2 is
// Swift-only (async/await) with no C/C++-callable surface at all. One shared
// transaction observer for the process's whole lifetime (StoreKit expects
// exactly this — register once at launch, not per-request), guarded by a
// mutex since paymentQueue:updatedTransactions: is not documented to always
// fire on the thread that called into StoreKit.
//
// SKPaymentQueue/SKPayment/SKPaymentTransaction were deprecated in iOS 18 /
// macOS 15 in favor of StoreKit 2's Product.purchase() — but not removed,
// and won't be for the foreseeable future (millions of shipped apps still
// use them). Since StoreKit 2 is Swift-only, this is the only IAP surface a
// C++ engine can call today; a future StoreKit 2 bridge would need a small
// Swift shim exporting @_cdecl functions, which is real, separate work.
// The pragmas below silence the resulting (expected, understood) warnings
// without silencing genuinely new deprecation warnings elsewhere in the file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#import <StoreKit/StoreKit.h>

@interface ThistleIAPObserver : NSObject <SKPaymentTransactionObserver, SKProductsRequestDelegate>
@end

namespace {
std::mutex g_iap_mutex;
bool g_iap_products_ready = false;
std::string g_iap_products_json_cache = "[]";
std::vector<std::string> g_iap_event_queue;             // each entry already JSON-encoded
ThistleIAPObserver* g_iap_observer = nil;
SKProductsRequest* g_iap_request = nil;                  // kept alive while a fetch is in flight
NSMutableDictionary<NSString*, SKProduct*>* g_iap_known_products = nil;  // id -> SKProduct, needed to actually purchase

void push_event_locked(NSString* kind, NSString* productId, NSString* transactionId, NSString* errorMessage) {
    NSDictionary* d = @{
        @"kind": kind ?: @"", @"product_id": productId ?: @"",
        @"transaction_id": transactionId ?: @"", @"error_message": errorMessage ?: @"",
    };
    NSData* data = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
    if (data) g_iap_event_queue.push_back(std::string(static_cast<const char*>(data.bytes), data.length));
}
} // namespace

@implementation ThistleIAPObserver

- (void)productsRequest:(SKProductsRequest *)request didReceiveResponse:(SKProductsResponse *)response {
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    if (!g_iap_known_products) g_iap_known_products = [NSMutableDictionary dictionary];
    NSMutableArray* arr = [NSMutableArray array];
    NSNumberFormatter* fmt = [[NSNumberFormatter alloc] init];
    fmt.numberStyle = NSNumberFormatterCurrencyStyle;
    for (SKProduct* p in response.products) {
        fmt.locale = p.priceLocale;
        g_iap_known_products[p.productIdentifier] = p;
        [arr addObject:@{
            @"id": p.productIdentifier ?: @"",
            @"title": p.localizedTitle ?: @"",
            @"description": p.localizedDescription ?: @"",
            @"price_string": [fmt stringFromNumber:p.price] ?: @"",
            @"price_value": p.price ?: @(0),
            @"currency_code": p.priceLocale.currencyCode ?: @"",
        }];
    }
    NSData* data = [NSJSONSerialization dataWithJSONObject:arr options:0 error:nil];
    g_iap_products_json_cache = data ? std::string(static_cast<const char*>(data.bytes), data.length) : "[]";
    g_iap_products_ready = true;
}

- (void)request:(SKRequest *)request didFailWithError:(NSError *)error {
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    g_iap_products_json_cache = "[]";   // "ready" with nothing — caller checks count vs what it asked for
    g_iap_products_ready = true;
}

- (void)paymentQueue:(SKPaymentQueue *)queue updatedTransactions:(NSArray<SKPaymentTransaction *> *)transactions {
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    for (SKPaymentTransaction* t in transactions) {
        NSString* kind = nil;
        NSString* err = @"";
        switch (t.transactionState) {
            case SKPaymentTransactionStatePurchased: kind = @"purchased"; break;
            case SKPaymentTransactionStateRestored:  kind = @"restored";  break;
            case SKPaymentTransactionStateDeferred:  kind = @"deferred";  break;
            case SKPaymentTransactionStateFailed:
                kind = @"failed";
                err = t.error ? t.error.localizedDescription : @"";
                [queue finishTransaction:t];   // no content to deliver — safe (required) to finish right away
                break;
            case SKPaymentTransactionStatePurchasing:
            default:
                continue;   // not a terminal state yet, nothing to report
        }
        push_event_locked(kind, t.payment.productIdentifier, t.transactionIdentifier, err);
    }
}

@end

extern "C" int thistle_iap_can_make_payments(void) {
    return [SKPaymentQueue canMakePayments] ? 1 : 0;
}

extern "C" void thistle_iap_fetch_products(const char* ids_json) {
    @autoreleasepool {
        if (!g_iap_observer) {
            g_iap_observer = [[ThistleIAPObserver alloc] init];
            [[SKPaymentQueue defaultQueue] addTransactionObserver:g_iap_observer];
        }
        NSData* raw = [NSData dataWithBytes:ids_json length:std::strlen(ids_json)];
        NSArray* ids = [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];
        NSMutableSet<NSString*>* set = [NSMutableSet set];
        for (NSString* s in ids) [set addObject:s];
        {
            std::lock_guard<std::mutex> lock(g_iap_mutex);
            g_iap_products_ready = false;
        }
        g_iap_request = [[SKProductsRequest alloc] initWithProductIdentifiers:set];
        g_iap_request.delegate = g_iap_observer;
        [g_iap_request start];
    }
}

extern "C" int thistle_iap_products_ready(void) {
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    return g_iap_products_ready ? 1 : 0;
}

extern "C" const char* thistle_iap_products_json(void) {
    static std::string out;
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    out = g_iap_products_json_cache;
    return out.c_str();
}

extern "C" void thistle_iap_purchase(const char* product_id) {
    @autoreleasepool {
        NSString* pid = [NSString stringWithUTF8String:product_id];
        SKProduct* product;
        {
            std::lock_guard<std::mutex> lock(g_iap_mutex);
            product = g_iap_known_products ? g_iap_known_products[pid] : nil;
            if (!product) {
                // Not a silent no-op: purchasing an id that was never fetched
                // is a real usage bug, so it surfaces as an immediate Failed
                // event instead of just doing nothing.
                push_event_locked(@"failed", pid, @"",
                    @"purchase() called before a successful iap_fetch_products() for this id");
                return;
            }
        }
        [[SKPaymentQueue defaultQueue] addPayment:[SKPayment paymentWithProduct:product]];
    }
}

extern "C" void thistle_iap_restore_purchases(void) {
    [[SKPaymentQueue defaultQueue] restoreCompletedTransactions];
}

extern "C" void thistle_iap_finish_transaction(const char* transaction_id) {
    @autoreleasepool {
        NSString* txId = [NSString stringWithUTF8String:transaction_id];
        for (SKPaymentTransaction* t in [SKPaymentQueue defaultQueue].transactions) {
            if ([t.transactionIdentifier isEqualToString:txId]) {
                [[SKPaymentQueue defaultQueue] finishTransaction:t];
                break;
            }
        }
    }
}

extern "C" const char* thistle_iap_poll_event_json(void) {
    static std::string out;
    std::lock_guard<std::mutex> lock(g_iap_mutex);
    if (g_iap_event_queue.empty()) return "";
    out = g_iap_event_queue.front();
    g_iap_event_queue.erase(g_iap_event_queue.begin());
    return out.c_str();
}
#pragma clang diagnostic pop

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
int         thistle_iap_can_make_payments(void) { return 0; }
void        thistle_iap_fetch_products(const char*) {}
int         thistle_iap_products_ready(void) { return 1; }
const char* thistle_iap_products_json(void) { return "[]"; }
void        thistle_iap_purchase(const char*) {}
void        thistle_iap_restore_purchases(void) {}
void        thistle_iap_finish_transaction(const char*) {}
const char* thistle_iap_poll_event_json(void) { return ""; }
void thistle_apple_poll_gamepad(ThistleGamepad* g) { std::memset(g, 0, sizeof(*g)); }
#endif
