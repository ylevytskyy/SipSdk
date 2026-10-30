//
// SipClientBaresipV4.mm
// Modern Baresip v4 adapter. Kept separate until SwiftPM is wired into the
// demo target; the current CocoaPods implementation remains unchanged.
//

#import "SipClient.h"
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <BaresipCore.h>

#include <errno.h>

static const int kDefaultPort = 5060;
static const unsigned kAsyncWorkerCount = 4;

@interface SipCall ()
@property (nonatomic) struct ua *ua;
@property (nonatomic) struct call *call;
@property (nonatomic, copy) NSString *cachedRemoteUri;
@end

@interface SipClient ()
@property (nonatomic) struct ua *ua;
@property (nonatomic, strong) NSThread *loopThread;
@property (nonatomic, strong) dispatch_semaphore_t loopStoppedSemaphore;
@property (nonatomic) BOOL loopThreadStarted;
@property (nonatomic) BOOL stackStarted;
@property (nonatomic) BOOL eventsRegistered;
@end

static NSMutableDictionary<NSValue *, SipCall *> *sipCallsCache(void)
{
    static NSMutableDictionary<NSValue *, SipCall *> *cache;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        cache = [[NSMutableDictionary alloc] init];
    });
    return cache;
}

static NSValue *sipCallCacheKey(struct call *call)
{
    return [NSValue valueWithPointer:call];
}

static SipCall *getSipCall(struct call *call, SipClient *sipSdk, NSString *remoteUri)
{
    if (call == nullptr) {
        return nil;
    }

    NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
    NSValue *key = sipCallCacheKey(call);

    @synchronized (cache) {
        SipCall *sipCall = cache[key];
        if (sipCall == nil) {
            sipCall = [[SipCall alloc] init];
            sipCall.ua = sipSdk.ua;
            sipCall.call = call;
            sipCall.cachedRemoteUri = remoteUri ?: @"";
            cache[key] = sipCall;
        }
        else {
            sipCall.ua = sipSdk.ua;
            sipCall.call = call;
            if (remoteUri.length > 0) {
                sipCall.cachedRemoteUri = remoteUri;
            }
        }
        return sipCall;
    }
}

static void invalidateSipCallsForClient(SipClient *sipSdk)
{
    NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
    @synchronized (cache) {
        for (NSValue *key in [cache.allKeys copy]) {
            SipCall *sipCall = cache[key];
            if (sipCall.ua == sipSdk.ua) {
                sipCall.call = nullptr;
                sipCall.ua = nullptr;
                [cache removeObjectForKey:key];
            }
        }
    }
}

static void ua_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
    SipClient *sipSdk = (__bridge SipClient *)arg;
    if (sipSdk == nil || event == nullptr) {
        return;
    }

    struct call *call = bevent_get_call(event);
    const char *peerUri = call ? call_peeruri(call) : nullptr;
    NSString *remoteUri = peerUri ? [NSString stringWithUTF8String:peerUri] : @"";
    SipCall *sipCall = getSipCall(call, sipSdk, remoteUri);

    NSLog(@"Baresip event %@ peer=%@", @(ev), remoteUri);

    switch (ev) {
        case BEVENT_REGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onWillRegister:)]) {
                    [delegate onWillRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_REGISTER_OK: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onDidRegister:)]) {
                    [delegate onDidRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_REGISTER_FAIL: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onFailedRegister:)]) {
                    [delegate onFailedRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_UNREGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onWillUnRegister:)]) {
                    [delegate onWillUnRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_CALL_INCOMING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallIncoming:)]) {
                    [delegate onCallIncoming:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_RINGING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallRinging:)]) {
                    [delegate onCallRinging:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_PROGRESS: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallProcess:)]) {
                    [delegate onCallProcess:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_ESTABLISHED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallEstablished:)]) {
                    [delegate onCallEstablished:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_CLOSED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallClosed:)]) {
                    [delegate onCallClosed:sipCall];
                }
            });

            if (call != nullptr) {
                NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
                @synchronized (cache) {
                    [cache removeObjectForKey:sipCallCacheKey(call)];
                }
            }
            sipCall.call = nullptr;
            sipCall.ua = nullptr;
            break;
        }

        case BEVENT_CALL_TRANSFER_FAILED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallTransferFailed:)]) {
                    [delegate onCallTransferFailed:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_DTMF_START: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallDtmfStart:)]) {
                    [delegate onCallDtmfStart:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_DTMF_END: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallDtmfEnd:)]) {
                    [delegate onCallDtmfEnd:sipCall];
                }
            });
            break;
        }

        default: {
            // Ignore events not represented by the legacy public delegate.
            break;
        }
    }
}

@implementation SipCall

- (NSString *)remoteUri
{
    return self.cachedRemoteUri ?: @"";
}

- (void)answer
{
    if (self.ua != nullptr && self.call != nullptr) {
        (void)ua_answer(self.ua, self.call, VIDMODE_OFF);
    }
}

- (void)hangup:(unsigned short)statusCode reason:(NSString *)reason
{
    if (self.ua != nullptr && self.call != nullptr) {
        const char *reasonText = reason.UTF8String ?: "";
        ua_hangup(self.ua, self.call, statusCode, reasonText);
    }
}

- (void)holdAnswer
{
    if (self.ua != nullptr && self.call != nullptr) {
        (void)ua_hold_answer(self.ua, self.call, VIDMODE_OFF);
    }
}

@end

@implementation SipClient

- (instancetype)initWithUsername:(NSString *)username domain:(NSString *)domain
{
    self = [super init];
    if (self) {
        _username = [username copy];
        _domain = [domain copy];
        _port = kDefaultPort;
    }
    return self;
}

- (NSString *)aor
{
    if (self.ua == nullptr) {
        return @"";
    }

    struct account *account = ua_account(self.ua);
    const char *aor = account ? account_aor(account) : nullptr;
    return aor ? [NSString stringWithUTF8String:aor] : @"";
}

- (int)start
{
    @synchronized (self) {
        if (self.stackStarted) {
            return 0;
        }

        int result = libre_init();
        if (result != 0) {
            return result;
        }

        NSString *documentsRoot =
            NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                NSUserDomainMask,
                                                YES).firstObject;
        if (documentsRoot == nil) {
            [self cleanupEngine];
            return EIO;
        }

        // Keep this configuration separate from Documents/config copied by
        // the legacy CocoaPods version.
        NSString *documentDirectory =
            [documentsRoot stringByAppendingPathComponent:@"baresip-v4"];
        NSFileManager *fileManager = [NSFileManager defaultManager];
        NSError *directoryError = nil;
        if (![fileManager createDirectoryAtPath:documentDirectory
                    withIntermediateDirectories:YES
                                     attributes:nil
                                          error:&directoryError]) {
            NSLog(@"Failed to create Baresip v4 config directory: %@", directoryError);
            [self cleanupEngine];
            return directoryError ? (int)directoryError.code : EIO;
        }

        NSString *configDestination =
            [documentDirectory stringByAppendingPathComponent:@"config"];
        if (![fileManager fileExistsAtPath:configDestination]) {
            NSBundle *mainBundle = [NSBundle mainBundle];
            NSString *bundledConfigPath =
                [mainBundle pathForResource:@"config" ofType:@"baresip-v4"];
            if (bundledConfigPath == nil) {
                // SwiftPM resource-only target is bundled separately from the
                // Objective-C++ target as SipSdk_SipSdkResources.bundle.
                NSURL *resourceBundleURL =
                    [mainBundle URLForResource:@"SipSdk_SipSdkResources"
                                  withExtension:@"bundle"];
                NSBundle *resourceBundle = resourceBundleURL
                    ? [NSBundle bundleWithURL:resourceBundleURL]
                    : nil;
                bundledConfigPath =
                    [resourceBundle pathForResource:@"config" ofType:@"baresip-v4"];
            }
            if (bundledConfigPath == nil) {
                NSLog(@"Baresip v4 config resource is missing from the app bundle.");
                [self cleanupEngine];
                return ENOENT;
            }

            NSError *copyError = nil;
            if (![fileManager copyItemAtPath:bundledConfigPath
                                      toPath:configDestination
                                       error:&copyError]) {
                NSLog(@"Failed to copy Baresip v4 config into Documents: %@", copyError);
                [self cleanupEngine];
                return copyError ? (int)copyError.code : EIO;
            }
        }

        result = conf_path_set(documentDirectory.UTF8String);
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        result = conf_configure();
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        // Mirrors upstream Baresip v4.12.0 src/main.c startup ordering.
        re_thread_async_init(kAsyncWorkerCount);

        result = baresip_init(conf_config());
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        // The packaged core is intentionally built without TLS support.
        result = ua_init("SipSdk", true, true, false);
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        result = bevent_register(ua_event_handler, (__bridge void *)self);
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }
        self.eventsRegistered = YES;

        result = conf_modules();
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        NSString *aor = [NSString stringWithFormat:@"sip:%@@%@:%d",
                         self.username, self.domain, self.port];
        result = ua_alloc(&_ua, aor.UTF8String);
        if (result != 0 || self.ua == nullptr) {
            [self cleanupEngine];
            return result != 0 ? result : EIO;
        }

        struct account *account = ua_account(self.ua);
        if (account == nullptr) {
            [self cleanupEngine];
            return EIO;
        }

        result = account_set_auth_user(account, self.username.UTF8String);
        if (result != 0) {
            [self cleanupEngine];
            return result;
        }

        if (self.password.length > 0) {
            result = account_set_auth_pass(account, self.password.UTF8String);
            if (result != 0) {
                [self cleanupEngine];
                return result;
            }
        }

        self.loopStoppedSemaphore = dispatch_semaphore_create(0);
        self.loopThread = [[NSThread alloc] initWithTarget:self
                                                  selector:@selector(runMainLoop)
                                                    object:nil];
        self.loopThread.name = @"SipSdk Baresip main loop";
        self.loopThreadStarted = YES;
        [self.loopThread start];
        self.stackStarted = YES;
        return 0;
    }
}

- (void)runMainLoop
{
    (void)re_main(nullptr);
    dispatch_semaphore_signal(self.loopStoppedSemaphore);
}

- (void)cleanupEngine
{
    // Called after the loop has stopped, or before its thread has been started.
    if (self.eventsRegistered) {
        bevent_unregister(ua_event_handler);
        self.eventsRegistered = NO;
    }

    invalidateSipCallsForClient(self);
    self.ua = nullptr;

    // Match upstream teardown order: UA, app modules, config, Baresip,
    // modules, async worker pool, and finally libre.
    ua_close();
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    self.loopThreadStarted = NO;
    self.stackStarted = NO;
    self.loopThread = nil;
    self.loopStoppedSemaphore = nil;
}

- (void)stop
{
    @synchronized (self) {
        if (!self.stackStarted && !self.loopThreadStarted) {
            return;
        }

        if (self.loopThreadStarted) {
            // re_cancel() makes re_main() return. Do not tear down Baresip
            // while its event loop is still executing.
            re_cancel();
            dispatch_semaphore_wait(self.loopStoppedSemaphore, DISPATCH_TIME_FOREVER);
        }

        [self cleanupEngine];
    }
}

- (int)registry
{
    if (self.ua == nullptr) {
        return EINVAL;
    }
    return ua_register(self.ua);
}

- (void)unregister
{
    if (self.ua != nullptr) {
        ua_unregister(self.ua);
    }
}

- (bool)isRegistered
{
    return self.ua != nullptr && ua_isregistered(self.ua);
}

- (SipCall *)makeCall:(NSString *)uri
{
    if (self.ua == nullptr || uri.length == 0) {
        NSLog(@"Cannot place a SIP call until the client is started and URI is non-empty.");
        return nil;
    }

    struct call *call = nullptr;
    int result = ua_connect(self.ua, &call, nullptr, uri.UTF8String, VIDMODE_OFF);
    if (result != 0 || call == nullptr) {
        NSLog(@"Failed to create SIP call (result %d).", result);
        return nil;
    }

    const char *peerUri = call_peeruri(call);
    NSString *remoteUri = peerUri ? [NSString stringWithUTF8String:peerUri] : uri;
    return getSipCall(call, self, remoteUri);
}

@end
