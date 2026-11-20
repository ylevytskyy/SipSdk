#import <Foundation/Foundation.h>
#import "SipMacCall.h"
#import "SipMacVideoView.h"

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SipMacRegistrationState) {
    SipMacRegistrationStateUnregistered = 0,
    SipMacRegistrationStateRegistering,
    SipMacRegistrationStateRegistered,
    SipMacRegistrationStateFailed
};

/// Status of a sent SIP MESSAGE; values match sip_message_status_t.
typedef NS_ENUM(NSInteger, SipMacMessageStatus) {
    SipMacMessageStatusSent = 0,
    SipMacMessageStatusFailed,
    SipMacMessageStatusDelivered,   // reserved: IMDN delivery notification
    SipMacMessageStatusDisplayed    // reserved: IMDN display (read) notification
};

@class SipMacClient;

@protocol SipMacClientDelegate <NSObject>
@optional
- (void)client:(SipMacClient *)client didReceiveMessageFrom:(NSString *)from contentType:(NSString *)contentType body:(NSData *)body;
- (void)client:(SipMacClient *)client message:(uint64_t)messageId didChangeStatus:(SipMacMessageStatus)status code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client didChangeRegistrationState:(SipMacRegistrationState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client didReceiveIncomingCall:(SipMacCall *)call;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didChangeState:(SipMacCallState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didUpdateMediaWidth:(int)width height:(int)height;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didReceiveDtmf:(NSString *)digit;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didUpdateStatsCodec:(NSString *)codec bitrateKbps:(uint32_t)bitrateKbps lossRate:(float)lossRate jitterMs:(uint32_t)jitterMs rttMs:(uint32_t)rttMs;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didChangeTransferStatus:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didReceiveTransferRequest:(NSString *)referToUri hasReplaces:(BOOL)hasReplaces;
- (void)client:(SipMacClient *)client didRejectIncomingCallBusyFrom:(NSString *)callerUri;
@end

@interface SipMacClient : NSObject

@property (nonatomic, weak) id<SipMacClientDelegate> delegate;
@property (nonatomic, readonly) BOOL isStarted;
@property (nonatomic, readonly) BOOL isRegistered;
@property (nonatomic, assign) BOOL isMicMuted;
@property (nonatomic, assign) int maxCalls;
@property (nonatomic, readonly, copy) NSString *username;
@property (nonatomic, readonly, copy) NSString *domain;
@property (nonatomic, readonly, copy) NSString *aor;

- (instancetype)initWithUsername:(NSString *)username domain:(NSString *)domain;

- (int)startWithConfigPath:(nullable NSString *)configPath;
- (void)stop;

- (int)registerWithPassword:(NSString *)password port:(int)port transport:(NSString *)transport;
- (int)registerWithUsername:(NSString *)user
                     domain:(NSString *)domain
                   password:(NSString *)password
                  transport:(NSString *)transport
                       port:(int)port
                displayName:(nullable NSString *)displayName
              outboundProxy:(nullable NSString *)outboundProxy
                 stunServer:(nullable NSString *)stunServer
                   mediaNat:(nullable NSString *)mediaNat
                regInterval:(uint32_t)regInterval;
- (int)registerWithUsername:(NSString *)user
                     domain:(NSString *)domain
                   password:(NSString *)password
                  transport:(NSString *)transport
                       port:(int)port
                displayName:(nullable NSString *)displayName
              outboundProxy:(nullable NSString *)outboundProxy
                 stunServer:(nullable NSString *)stunServer
                   mediaNat:(nullable NSString *)mediaNat
                regInterval:(uint32_t)regInterval
                  tlsCaFile:(nullable NSString *)tlsCaFile
              verifyTlsCert:(BOOL)verifyTlsCert
                   mediaEnc:(nullable NSString *)mediaEnc;
- (int)unregisterAccount;
- (int)resetTransports:(BOOL)reRegister;

- (nullable SipMacCall *)makeCallToUri:(NSString *)uri withVideo:(BOOL)withVideo;

// Messaging (SIP MESSAGE). Return the local message id, or 0 when the message was not queued.
- (uint64_t)sendMessageToUri:(NSString *)uri text:(NSString *)text;
- (uint64_t)sendMessageToUri:(NSString *)uri body:(NSData *)body contentType:(NSString *)contentType;

- (void)setRemoteVideoView:(nullable SipMacVideoView *)videoView;
- (void)setLocalPreviewView:(nullable SipMacVideoView *)previewView;

// Local Video Preview
- (int)startLocalPreview;
- (int)stopLocalPreview;
- (BOOL)isLocalPreviewActive;

// Audio Devices
- (NSArray<NSString *> *)availableAudioInputDevices;
- (nullable NSString *)selectedAudioInputDevice;
- (void)selectAudioInputDeviceByName:(NSString *)deviceName;

- (NSArray<NSString *> *)availableAudioOutputDevices;
- (nullable NSString *)selectedAudioOutputDevice;
- (void)selectAudioOutputDeviceByName:(NSString *)deviceName;

// Video Input (Camera)
- (void)useSyntheticCamera:(BOOL)useSynthetic;
- (NSArray<NSString *> *)availableCameras;
- (nullable NSString *)selectedCamera;
- (void)selectCameraByName:(NSString *)cameraName;

// Video Output (Display)
- (NSArray<NSString *> *)availableVideoOutputDevices;
- (nullable NSString *)selectedVideoOutputDevice;
- (void)selectVideoOutputDeviceByName:(NSString *)deviceName;

@end

NS_ASSUME_NONNULL_END
