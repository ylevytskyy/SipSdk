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

typedef NS_ENUM(NSInteger, SipMacRecordingMode) {
    SipMacRecordingModeAudioStereo = 0,
    SipMacRecordingModeAudioMono = 1,
    SipMacRecordingModeAudioVideo = 2
};

typedef NS_ENUM(NSInteger, SipMacRecordingState) {
    SipMacRecordingStateIdle = 0,
    SipMacRecordingStateRecording = 1,
    SipMacRecordingStateFailed = 2
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
@property (nonatomic, readonly) BOOL isRecording;
@property (nonatomic, readonly, copy) NSString *recordingPath;
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
- (int)startRecordingToPath:(NSString *)path mode:(SipMacRecordingMode)mode;
- (int)stopRecording;

- (void)invalidate;
- (struct sip_call_t *)handle;

@end

NS_ASSUME_NONNULL_END
