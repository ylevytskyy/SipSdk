#import "SipMacCall.h"
#include <sip_abi.h>
#include <mutex>

@implementation SipMacCall {
    struct sip_call_t *_handle;
    std::mutex _mutex;
    NSString *_cachedCallId;
    NSString *_cachedRemoteUri;
}

- (instancetype)initWithHandle:(struct sip_call_t *)handle
{
    self = [super init];
    if (self) {
        _handle = handle;
        if (handle) {
            const char *cid = sip_call_get_id(handle);
            _cachedCallId = cid ? [NSString stringWithUTF8String:cid] : @"";
            const char *uri = sip_call_get_remote_uri(handle);
            _cachedRemoteUri = uri ? [NSString stringWithUTF8String:uri] : @"";
        } else {
            _cachedCallId = @"";
            _cachedRemoteUri = @"";
        }
    }
    return self;
}

- (struct sip_call_t *)handle
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle;
}

- (void)invalidate
{
    std::lock_guard<std::mutex> lock(_mutex);
    _handle = nullptr;
}

- (NSString *)callId
{
    return _cachedCallId;
}

- (NSString *)remoteUri
{
    return _cachedRemoteUri;
}

- (BOOL)hasVideo
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_has_video(_handle) : NO;
}

- (BOOL)isOutgoing
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_is_outgoing(_handle) : NO;
}

- (BOOL)isOnHold
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_is_on_hold(_handle) : NO;
}

- (BOOL)isMuted
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_is_muted(_handle) : NO;
}

- (BOOL)isClosed
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_is_closed(_handle) : YES;
}

- (uint32_t)duration
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _handle ? sip_call_get_duration(_handle) : 0;
}

- (int)answerWithVideo:(BOOL)withVideo
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return -1;
    return sip_call_answer(_handle, withVideo);
}

- (int)hangupWithCode:(int)code reason:(NSString *)reason
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return -1;
    return sip_call_hangup(_handle, code, reason ? [reason UTF8String] : "Hangup");
}

- (int)setHold:(BOOL)hold
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return -1;
    return sip_call_set_hold(_handle, hold);
}

- (int)setMuted:(BOOL)muted
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return -1;
    return sip_call_set_muted(_handle, muted);
}

- (int)sendDtmf:(NSString *)digits
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle || !digits) return -1;
    return sip_call_send_dtmf(_handle, [digits UTF8String]);
}

- (int)requestKeyframe
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_handle) return -1;
    return sip_call_request_keyframe(_handle);
}

@end
