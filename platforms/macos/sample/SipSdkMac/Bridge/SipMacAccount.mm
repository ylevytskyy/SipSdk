#import "SipMacAccount.h"
#import "SipMacCall.h"
#import "SipMacClientInternal.h"
#include <sip_abi.h>

#include <cerrno>
#include <mutex>

@implementation SipMacAccount {
    struct sip_account_t *_handle;
    __weak SipMacClient *_client;
    BOOL _ownsHandle;
    std::mutex _mutex;
    uint64_t _cachedAccountId;
    NSString *_cachedAor;
    NSString *_cachedUser;
    NSString *_cachedDomain;
}

- (instancetype)initWithHandle:(struct sip_account_t *)handle
                        client:(nullable SipMacClient *)client
                    ownsHandle:(BOOL)ownsHandle
{
    self = [super init];
    if (self) {
        _handle = handle;
        _client = client;
        _ownsHandle = ownsHandle;
        if (handle) {
            _cachedAccountId = sip_account_get_id(handle);
            const char *aor = sip_account_get_aor(handle);
            const char *user = sip_account_get_user(handle);
            const char *domain = sip_account_get_domain(handle);
            _cachedAor = aor ? [NSString stringWithUTF8String:aor] : @"";
            _cachedUser = user ? [NSString stringWithUTF8String:user] : @"";
            _cachedDomain = domain ? [NSString stringWithUTF8String:domain] : @"";
        } else {
            _cachedAccountId = 0;
            _cachedAor = @"";
            _cachedUser = @"";
            _cachedDomain = @"";
        }
    }
    return self;
}

- (void)dealloc
{
    [self invalidate];
}

- (nullable struct sip_account_t *)handle
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle;
}

- (void)invalidate
{
    struct sip_account_t *toDestroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_handle && _ownsHandle) {
            toDestroy = _handle;
        }
        _handle = nullptr;
        _ownsHandle = NO;
    }
    if (toDestroy) {
        sip_account_destroy(toDestroy);
    }
}

- (uint64_t)accountId
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_account_get_id(_handle) : _cachedAccountId;
}

- (NSString *)aor
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_handle) {
        const char *a = sip_account_get_aor(_handle);
        if (a && a[0] != '\0') {
            return [NSString stringWithUTF8String:a] ?: _cachedAor;
        }
    }
    return _cachedAor;
}

- (NSString *)user
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_handle) {
        const char *u = sip_account_get_user(_handle);
        if (u && u[0] != '\0') {
            return [NSString stringWithUTF8String:u] ?: _cachedUser;
        }
    }
    return _cachedUser;
}

- (NSString *)domain
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_handle) {
        const char *d = sip_account_get_domain(_handle);
        if (d && d[0] != '\0') {
            return [NSString stringWithUTF8String:d] ?: _cachedDomain;
        }
    }
    return _cachedDomain;
}

- (BOOL)isRegistered
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_account_is_registered(_handle) : NO;
}

- (SipMacRegistrationState)regState
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return SipMacRegistrationStateUnregistered;
    return static_cast<SipMacRegistrationState>(sip_account_get_reg_state(_handle));
}

- (int)reregister
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return EINVAL;
    return sip_account_reregister(_handle);
}

- (int)unregister
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return EINVAL;
    return sip_account_unregister(_handle);
}

- (nullable SipMacCall *)dial:(NSString *)uri withVideo:(BOOL)withVideo
{
    if (!uri || uri.length == 0) return nil;
    struct sip_account_t *h = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        h = _handle;
    }
    if (!h) return nil;

    sip_call_t *callHandle = nullptr;
    int rc = sip_account_dial(h, [uri UTF8String], withVideo, &callHandle);
    if (rc != 0 || !callHandle) return nil;

    SipMacClient *client = _client;
    if (client) {
        return [client getOrCreateCall:callHandle];
    }
    return [[SipMacCall alloc] initWithHandle:callHandle];
}

- (uint64_t)sendMessageToUri:(NSString *)uri text:(NSString *)text
{
    NSData *body = [text dataUsingEncoding:NSUTF8StringEncoding];
    return [self sendMessageToUri:uri body:body ?: [NSData data] contentType:@"text/plain"];
}

- (uint64_t)sendMessageToUri:(NSString *)uri body:(NSData *)body contentType:(NSString *)contentType
{
    if (uri.length == 0 || body.length == 0 || contentType.length == 0) return 0;
    struct sip_account_t *h = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        h = _handle;
    }
    if (!h) return 0;

    uint64_t messageId = 0;
    int rc = sip_account_send_message(h,
                                      [uri UTF8String],
                                      [contentType UTF8String],
                                      static_cast<const uint8_t *>(body.bytes),
                                      body.length,
                                      &messageId);
    return rc == 0 ? messageId : 0;
}

@end
