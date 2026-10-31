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

NS_ASSUME_NONNULL_BEGIN

@class SipCall;
@class SipClient;
@class SipVideoView;

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

- (BOOL)hasVideoCodecRegistered;
- (BOOL)hasVideoSourceRegistered;
- (BOOL)hasVideoDisplayRegistered;
+ (BOOL)testH264RoundTripWithWidth:(int)width height:(int)height;
@end

/// SIP Call
@interface SipCall : NSObject
@property (nonatomic, readonly) NSString* remoteUri;
@property (nonatomic, readonly) NSString* callId;
@property (nonatomic, readonly) NSUUID* uuid;
@property (nonatomic, readonly) BOOL hasVideo;
@property (nonatomic, readonly) BOOL isOutgoing;
@property (nonatomic, readonly, getter=isOnHold) BOOL onHold;
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
