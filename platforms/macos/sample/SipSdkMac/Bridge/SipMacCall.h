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
@property (nonatomic, readonly) BOOL isSecure;
@property (nonatomic, readonly) BOOL isInConference;
@property (nonatomic, readonly) uint32_t duration;
@property (nonatomic, readonly, copy) NSString *audioCodec;
@property (nonatomic, readonly) uint32_t audioBitrateKbps;
@property (nonatomic, readonly) float audioLossRate;
@property (nonatomic, readonly) uint32_t audioJitterMs;

- (instancetype)initWithHandle:(struct sip_call_t *)handle;

- (int)answerWithVideo:(BOOL)withVideo;
- (int)hangupWithCode:(int)code reason:(NSString *)reason;
- (int)setHold:(BOOL)hold;
- (int)setMuted:(BOOL)muted;
- (int)sendDtmf:(NSString *)digits;
- (int)requestKeyframe;
- (int)transferToUri:(NSString *)targetUri;
- (int)attendedTransferWithCall:(SipMacCall *)consultCall;
- (int)acceptTransfer:(BOOL)accept;

- (void)invalidate;
- (struct sip_call_t *)handle;

@end

NS_ASSUME_NONNULL_END
