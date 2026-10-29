//
//  SipClient.m
//  TareSIPDemo
//
//  Created by Yuriy Levytskyy on 2/27/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

#import "SipClient.h"

#define HAVE__BOOL

#import <re.h>
#import <baresip.h>

static const int kDefaultPort = 5060;

//static dispatch_once_t eventDispatchQueueToken;
//static dispatch_queue_t eventDispatchQueue;

@interface SipCall ()
@property (nonatomic) struct ua *ua;
@property (nonatomic) struct call* call;
@property (nonatomic) int baresipResultCode;

@property (nonatomic) NSString* cachedRemoteUri;
@end

@implementation SipCall
- (instancetype)init {
    self = [super init];
    return self;
}

- (NSString*)remoteUri {
    return self.cachedRemoteUri;
}

- (void)answer {
    if (self.ua && self.call) {
        ua_answer(self.ua, self.call);
    }
}

- (void)hangup:(unsigned short)statusCode reason:(NSString*)reason {
    if (self.ua && self.call) {
        ua_hangup(self.ua, self.call, statusCode, [reason cStringUsingEncoding:NSUTF8StringEncoding]);
    }
}

- (void)holdAnswer {
    if (self.ua && self.call) {
        ua_hold_answer(self.ua, self.call);
    }
}
@end

@interface SipClient()
@property (nonatomic) struct ua *ua;
@end

static NSMutableDictionary<NSValue *, SipCall *> *sipCallsCache(void) {
    static NSMutableDictionary<NSValue *, SipCall *> *cache;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        cache = [[NSMutableDictionary alloc] init];
    });
    return cache;
}

static NSValue *sipCallCacheKey(struct call *call) {
    return [NSValue valueWithPointer:call];
}

static SipCall *getSipCall(struct call *call, SipClient *sipSdk, NSString *remoteUri) {
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
        return sipCall;
    }
}

static void ua_event_handler(struct ua *ua, enum ua_event ev,
    struct call *call, const char *prm, void *arg)
{
    NSString *eventParameter = prm ? [NSString stringWithUTF8String:prm] : @"";
    NSLog(@"ua_event_handler %@ %@", @(ev), eventParameter);

    SipClient *sipSdk = (__bridge SipClient *)arg;
    SipCall *sipCall = getSipCall(call, sipSdk, eventParameter);
    
    switch (ev) {
        case UA_EVENT_REGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onWillRegister:)]) {
                    [sipSdk.delegate onWillRegister:sipSdk];
                }
            });
        }
            break;
            
        case UA_EVENT_REGISTER_OK: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onDidRegister:)]) {
                    [sipSdk.delegate onDidRegister:sipSdk];
                }
            });
        }
            break;

        case UA_EVENT_REGISTER_FAIL: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onFailedRegister:)]) {
                    [sipSdk.delegate onFailedRegister:sipSdk];
                }
            });
        }
            break;

        case UA_EVENT_UNREGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onWillUnRegister:)]) {
                    [sipSdk.delegate onWillUnRegister:sipSdk];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_INCOMING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallIncoming:)]) {
                    [sipSdk.delegate onCallIncoming:sipCall];
                }
            });
        }
            break;

        case UA_EVENT_CALL_RINGING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallRinging:)]) {
                    [sipSdk.delegate onCallRinging:sipCall];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_PROGRESS: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallProcess:)]) {
                    [sipSdk.delegate onCallProcess:sipCall];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_ESTABLISHED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallEstablished:)]) {
                    [sipSdk.delegate onCallEstablished:sipCall];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_CLOSED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallClosed:)]) {
                    [sipSdk.delegate onCallClosed:sipCall];
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
        }
            break;
            
        case UA_EVENT_CALL_TRANSFER_FAILED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallTransferFailed:)]) {
                    [sipSdk.delegate onCallTransferFailed:sipCall];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_DTMF_START: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallDtmfStart:)]) {
                    [sipSdk.delegate onCallDtmfStart:sipCall];
                }
            });
        }
            break;
            
        case UA_EVENT_CALL_DTMF_END: {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([sipSdk.delegate respondsToSelector:@selector(onCallDtmfEnd:)]) {
                    [sipSdk.delegate onCallDtmfEnd:sipCall];
                }
            });
        }
            break;

        default:
            // Ignore engine events that this wrapper does not expose.
            break;
    }
}

@implementation SipClient
//+ (void)load {
//    dispatch_once (&eventDispatchQueueToken, ^{
//        eventDispatchQueue = dispatch_queue_create("SIPSDK Event Queue", NULL);
//    });
//}

- (NSString*)aor {
    return @(ua_aor(self.ua));
}

- (instancetype)initWithUsername:(NSString*)username domain:(NSString*)domain {
    self = [super init];
    if (self) {
        _username = username;
        _domain = domain;
        
        _port = kDefaultPort;
    }
    return self;
}

- (int)start {
    int result = libre_init();
    if (result != 0) {
        return result;
    }
    
    // Initialize dynamic modules.
    mod_init();
    
    NSString *documentDirectory = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (documentDirectory == nil) {
        mod_close();
        libre_close();
        return -1;
    }

    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSString *configDestination = [documentDirectory stringByAppendingPathComponent:@"config"];
    if (![fileManager fileExistsAtPath:configDestination]) {
        NSString *bundledConfigPath = [[NSBundle mainBundle] pathForResource:@"config" ofType:nil];
        if (bundledConfigPath == nil) {
            NSLog(@"BareSIP config resource is missing from the app bundle.");
            mod_close();
            libre_close();
            return -1;
        }

        NSError *copyError = nil;
        if (![fileManager copyItemAtPath:bundledConfigPath toPath:configDestination error:&copyError]) {
            NSLog(@"Failed to copy BareSIP config into Documents: %@", copyError);
            mod_close();
            libre_close();
            return -1;
        }
    }

    conf_path_set([documentDirectory cStringUsingEncoding:NSUTF8StringEncoding]);

    result = conf_configure();
    if (result != 0) {
        return result;
    }

    // Initialize the SIP stack.
    result = ua_init("SIP", 1, 1, 1, 0);
    if (result != 0) {
        return result;
    }

    // Register UA event handler
    result = uag_event_register(ua_event_handler, (__bridge void *)(self));
    if (result != 0) {
        return result;
    }
    
    result = conf_modules();
    if (result != 0) {
        return result;
    }

    NSString* aor;
    if (self.password) {
        aor = [NSString stringWithFormat:@"sip:%@:%@@%@:%@", self.username, self.password, self.domain, @(self.port)];
    } else {
        aor = [NSString stringWithFormat:@"sip:%@@%@:%@", self.username, self.domain, @(self.port)];
    }
    
    // Start user agent.
    result = ua_alloc(&_ua, [aor cStringUsingEncoding:NSUTF8StringEncoding]);
    if (result != 0) {
        return result;
    }
    
    // Start the main loop.
    NSThread *thread = [[NSThread alloc] initWithTarget:self selector:@selector(_run) object:nil];
    [thread start];
    
    return 0;
}

- (void)stop {
    if (!self.ua) {
        return;
    }
    
    mem_deref(self.ua);
    self.ua = nil;
    
    ua_close();
    
    uag_event_unregister(ua_event_handler);
    
    mod_close();
    
    // Close
    libre_close();
    
    // Check for memory leaks.
    tmr_debug();
    mem_debug();
}

- (int)registry {
    if (!self.ua) {
        return -1;
    }

    int result = ua_register(self.ua);
    if (result != 0) {
        return result;
    }
    return 0;
}

- (void)unregister {
    if (self.ua) {
        ua_unregister(self.ua);
    }
}

- (bool)isRegistered {
    return self.ua ? ua_isregistered(self.ua) : false;
}

- (SipCall*)makeCall:(NSString*)uri {
    SipCall* sipCall = [[SipCall alloc] init];
    sipCall.ua = self.ua;
    sipCall.cachedRemoteUri = uri;

    if (!self.ua) {
        sipCall.baresipResultCode = -1;
        return sipCall;
    }

    struct call *call = nullptr;
    int result = ua_connect(self.ua, &call, nil, [uri cStringUsingEncoding:NSUTF8StringEncoding], nil, VIDMODE_OFF);
    if (result != 0) {
        sipCall.baresipResultCode = result;
    } else {
        sipCall.call = call;
        if (call != nullptr) {
            NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
            @synchronized (cache) {
                cache[sipCallCacheKey(call)] = sipCall;
            }
        }
    }

    return sipCall;
}

-(void)_run {
    // Start the main loop.
    re_main(nil);
}

@end
