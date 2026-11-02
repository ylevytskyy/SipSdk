#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SipMacCallState) {
    SipMacCallStateIncoming = 0,
    SipMacCallStateRinging,
    SipMacCallStateConnecting,
    SipMacCallStateEstablished,
    SipMacCallStateHold,
    SipMacCallStateClosed
};

struct sip_call_t;

@interface SipMacCall : NSObject

@property (nonatomic, readonly, copy) NSString *callId;
@property (nonatomic, readonly, copy) NSString *remoteUri;
@property (nonatomic, readonly) BOOL hasVideo;
@property (nonatomic, readonly) BOOL isOutgoing;
@property (nonatomic, readonly) BOOL isOnHold;
@property (nonatomic, readonly) BOOL isMuted;
@property (nonatomic, readonly) BOOL isClosed;
@property (nonatomic, readonly) uint32_t duration;

- (instancetype)initWithHandle:(struct sip_call_t *)handle;

- (int)answerWithVideo:(BOOL)withVideo;
- (int)hangupWithCode:(int)code reason:(NSString *)reason;
- (int)setHold:(BOOL)hold;
- (int)setMuted:(BOOL)muted;
- (int)sendDtmf:(NSString *)digits;
- (int)requestKeyframe;

- (void)invalidate;
- (struct sip_call_t *)handle;

@end

NS_ASSUME_NONNULL_END
