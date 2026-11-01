//
//  SipClient.h
//  SipSdk
//
//  Created by Yuriy Levytskyy on 2/27/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import "sip_abi.h"

NS_ASSUME_NONNULL_BEGIN

@class SipCall;
@class SipClient;
@class SipVideoView;
@class IosVideoCaptureDriver;
@class IosVideoRendererDriver;
@class IosVideoCodecDriver;

typedef NS_ENUM(NSInteger, SipCameraPosition) {
    SipCameraPositionFront = 0,
    SipCameraPositionBack = 1
};

/// Native iOS video rendering view for local camera preview and remote SIP video streams.
@interface SipVideoView : UIView
@property (nonatomic, readonly) CGSize videoSize;
@property (nonatomic, readonly) NSUInteger renderedFrameCount;

- (void)renderYUV420WithY:(const uint8_t *)yPlane
                        u:(const uint8_t *)uPlane
                        v:(const uint8_t *)vPlane
                  yStride:(int)yStride
                  uStride:(int)uStride
                  vStride:(int)vStride
                    width:(int)width
                   height:(int)height;
- (void)renderTestPatternWithWidth:(int)width height:(int)height;
- (void)clearVideo;
@end

/// Native iOS video capture driver bridging AVCaptureSession (device) and synthetic generator (simulator) to sip_video_capture_driver_t.
@interface IosVideoCaptureDriver : NSObject
@property (nonatomic, weak, nullable) SipClient *client;
- (instancetype)initWithClient:(nullable SipClient *)client;
- (sip_video_capture_driver_t)cDriver;
- (int)startWithWidth:(int)width height:(int)height fps:(int)fps;
- (void)stop;
- (void)switchCamera;
- (void)setCamera:(int)position;
- (void)setEnabled:(BOOL)enabled;
- (BOOL)isEnabled;
- (void)deliverSyntheticFrame;
- (void)deliverCustomFrame:(const sip_video_frame_t *)frame;
- (void)deliverSampleBuffer:(CMSampleBufferRef)sampleBuffer;
@end

/// Native iOS video renderer driver bridging decoded remote frames to SipVideoView.
@interface IosVideoRendererDriver : NSObject
@property (nonatomic, weak, nullable) SipVideoView *videoView;
- (instancetype)initWithVideoView:(nullable SipVideoView *)videoView;
- (sip_video_renderer_driver_t)cDriver;
- (void)renderFrame:(const sip_video_frame_t *)frame;
- (void)onStreamResizedWithWidth:(int)width height:(int)height;
- (void)clear;
@end

/// Native iOS video codec driver bridging Apple VideoToolbox H.264 to sip_video_codec_driver_t.
@interface IosVideoCodecDriver : NSObject
- (sip_video_codec_driver_t)cDriver;
- (int)registerCodec;
- (void)unregisterCodec;
@end

/// SIP Events
@protocol SipClientDelegate<NSObject>
@optional
- (void)onWillRegister:(SipClient*)sipSdk;
- (void)onDidRegister:(SipClient*)sipSdk;
- (void)onFailedRegister:(SipClient*)sipSdk;
- (void)onWillUnRegister:(SipClient*)sipSdk;

- (void)onCallIncoming:(SipCall*)sipCall;
- (void)onCallRinging:(SipCall*)sipCall;
- (void)onCallProcess:(SipCall*)sipCall;
- (void)onCallEstablished:(SipCall*)sipCall;
- (void)onCallClosed:(SipCall*)sipCall;
- (void)onCallTransferFailed:(SipCall*)sipCall;
- (void)onCallDtmfStart:(SipCall*)sipCall;
- (void)onCallDtmfEnd:(SipCall*)sipCall;

- (void)onCallHoldChanged:(SipCall*)sipCall isOnHold:(BOOL)isOnHold;
- (void)onCallVideoSizeChanged:(SipCall*)sipCall width:(int)width height:(int)height;
- (void)onDtmfReceived:(SipCall*)sipCall digit:(NSString*)digit;
@end

/// SIP Client
@interface SipClient : NSObject
@property (readonly, nonatomic, nonnull) NSString* username;
@property (readonly, nonatomic, nonnull) NSString* domain;
@property (nonatomic, nullable) NSString* password;
@property (nonatomic, copy, nullable) NSString* displayName;
@property (nonatomic, copy, nullable) NSString* transport;
@property (nonatomic) int port;

@property (readonly, nonatomic) NSString* aor;

@property (nonatomic, weak, nullable) id<SipClientDelegate> delegate;

@property (nonatomic, assign) BOOL videoEnabledByDefault;
@property (nonatomic, assign, getter=isMicrophoneMuted) BOOL microphoneMuted;
@property (nonatomic, assign, getter=isSpeakerEnabled) BOOL speakerEnabled;
@property (nonatomic, assign, getter=isLocalVideoEnabled) BOOL localVideoEnabled;
@property (nonatomic, assign) SipCameraPosition cameraPosition;

@property (nonatomic, strong, nullable) SipVideoView* remoteVideoView;
@property (nonatomic, strong, nullable) SipVideoView* localVideoView;

@property (nonatomic, readonly, nullable) sip_client_t *coreClient;
@property (nonatomic, readonly, strong) IosVideoCaptureDriver *captureDriver;
@property (nonatomic, readonly, strong) IosVideoRendererDriver *remoteRendererDriver;
@property (nonatomic, readonly, strong) IosVideoRendererDriver *localRendererDriver;
@property (nonatomic, readonly, strong) IosVideoCodecDriver *codecDriver;

- (void)registerNativeMediaDriversWithCore;

- (instancetype)initWithUsername:(NSString*)username domain:(NSString*)domain;

- (int)start;
- (void)stop;

- (int)registry;
- (void)unregister;
- (bool)isRegistered;

- (nullable SipCall *)makeCall:(NSString *)uri;
- (nullable SipCall *)makeCall:(NSString *)uri withVideo:(BOOL)withVideo;

- (void)setMuted:(BOOL)muted;
- (void)setSpeakerphone:(BOOL)enabled;
- (void)switchCamera;

/// Standalone local camera preview rendered into `localVideoView` without an
/// active SIP call. Falls back to the synthetic pattern where no camera is
/// available (e.g. iOS Simulator).
- (void)startLocalVideoPreview;
- (void)stopLocalVideoPreview;
@property (nonatomic, readonly, getter=isLocalVideoPreviewActive) BOOL localVideoPreviewActive;
/// YES while the hardware AVCaptureSession is delivering frames (NO when the
/// preview is fed by the synthetic pattern).
@property (nonatomic, readonly, getter=isLiveCameraFeedActive) BOOL liveCameraFeedActive;

- (BOOL)hasVideoCodecRegistered;
- (BOOL)hasVideoSourceRegistered;
- (BOOL)hasVideoDisplayRegistered;
+ (BOOL)testH264RoundTripWithWidth:(int)width height:(int)height;
/// Test hook: drops one RTP packet mid-stream. Returns YES when the decoder
/// reports the loss (so Baresip requests a keyframe), hides frames until the
/// next IDR, and decodes again after it.
+ (BOOL)testH264PacketLossRecoveryWithWidth:(int)width height:(int)height;
/// Test hook: sends one synthetic H.264 video frame packetized as RFC 6184 RTP
/// over UDP to a target loopback port.
+ (BOOL)sendH264TestFrameToPort:(uint16_t)port
                           ssrc:(uint32_t)ssrc
                            seq:(uint16_t *)seq
                             ts:(uint32_t)ts
                          width:(int)width
                         height:(int)height
                            idr:(BOOL)idr;
@end

/// SIP Call
@interface SipCall : NSObject
@property (nonatomic, readonly) NSString* remoteUri;
@property (nonatomic, readonly) NSString* callId;
@property (nonatomic, readonly) NSUUID* uuid;
@property (nonatomic, readonly) BOOL hasVideo;
@property (nonatomic, readonly) BOOL isOutgoing;
@property (nonatomic, readonly, getter=isOnHold) BOOL onHold;
@property (nonatomic, readonly, getter=isClosed) BOOL closed;
@property (nonatomic, readonly) uint32_t durationSeconds;

- (void)answer;
- (void)answerWithVideo:(BOOL)withVideo;
- (void)hangup:(unsigned short)statusCode reason:(NSString*)reason;
- (void)holdAnswer;
- (int)setHold:(BOOL)hold;
- (int)sendDtmf:(NSString*)digits;
- (int)setVideoEnabled:(BOOL)enabled;
- (void)requestKeyframe;
@end

NS_ASSUME_NONNULL_END
