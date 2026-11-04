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

@class SipMacClient;

@protocol SipMacClientDelegate <NSObject>
@optional
- (void)client:(SipMacClient *)client didChangeRegistrationState:(SipMacRegistrationState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client didReceiveIncomingCall:(SipMacCall *)call;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didChangeState:(SipMacCallState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didUpdateMediaWidth:(int)width height:(int)height;
- (void)client:(SipMacClient *)client call:(SipMacCall *)call didReceiveDtmf:(NSString *)digit;
@end

@interface SipMacClient : NSObject

@property (nonatomic, weak) id<SipMacClientDelegate> delegate;
@property (nonatomic, readonly) BOOL isStarted;
@property (nonatomic, readonly) BOOL isRegistered;
@property (nonatomic, assign) BOOL isMicMuted;
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
- (int)unregisterAccount;
- (int)resetTransports:(BOOL)reRegister;

- (nullable SipMacCall *)makeCallToUri:(NSString *)uri withVideo:(BOOL)withVideo;

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
