#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CallKit/CallKit.h>
#import <AVFoundation/AVFoundation.h>
#import "SipClient.h"
#import "SipSdkAppStores-Swift.h"

static int gFailures = 0;

static void check(BOOL condition, NSString *name, NSString *detail)
{
    if (condition) {
        printf("PASS: %s%s%s\n",
               name.UTF8String,
               detail.length > 0 ? " — " : "",
               detail.UTF8String ?: "");
    } else {
        printf("FAIL: %s%s%s\n",
               name.UTF8String,
               detail.length > 0 ? " — " : "",
               detail.UTF8String ?: "");
        gFailures++;
    }
}

int main(int argc, char *argv[])
{
    @autoreleasepool {
        // Ensure NSDocumentDirectory exists in the spawned simulator environment
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (docs.length > 0) {
            [[NSFileManager defaultManager] createDirectoryAtPath:docs
                                      withIntermediateDirectories:YES
                                                       attributes:nil
                                                            error:nil];
        }

        // 1. Test SipClient initialization and default properties
        SipClient *client = [[SipClient alloc] initWithUsername:@"alice" domain:@"127.0.0.1"];
        client.password = @"secret";
        client.displayName = @"Alice Video";
        client.port = 5060;

        check([client.username isEqualToString:@"alice"], @"SipClient stores username", client.username);
        check([client.domain isEqualToString:@"127.0.0.1"], @"SipClient stores domain", client.domain);
        check(client.videoEnabledByDefault == YES, @"SipClient enables video by default", @"");
        check(client.cameraPosition == SipCameraPositionFront, @"SipClient defaults to front camera", @"");

        // 2. Test SipVideoView creation and frame rendering
        SipVideoView *remoteView = [[SipVideoView alloc] initWithFrame:CGRectMake(0, 0, 320, 240)];
        SipVideoView *localView = [[SipVideoView alloc] initWithFrame:CGRectMake(0, 0, 120, 160)];
        client.remoteVideoView = remoteView;
        client.localVideoView = localView;

        [remoteView renderTestPatternWithWidth:640 height:480];
        check(CGSizeEqualToSize(remoteView.videoSize, CGSizeMake(640, 480)),
              @"SipVideoView renders frame and updates videoSize",
              NSStringFromCGSize(remoteView.videoSize));
        check(remoteView.renderedFrameCount >= 1,
              @"SipVideoView increments renderedFrameCount",
              [NSString stringWithFormat:@"count=%lu", (unsigned long)remoteView.renderedFrameCount]);

        // 3. Start Baresip v4 stack and verify video codec, source, and display registration
        int startRes = [client start];
        check(startRes == 0, @"SipClient starts Baresip v4 engine cleanly",
              [NSString stringWithFormat:@"rc=%d aor=%@", startRes, client.aor]);
        check([client.aor containsString:@"sip:alice@127.0.0.1"],
              @"SipClient formats AOR from username and domain", client.aor);
        check([client hasVideoCodecRegistered],
              @"Baresip v4 registers VideoToolbox H264 vidcodec", @"H264");
        check([client hasVideoSourceRegistered],
              @"Baresip v4 registers avcapture vidsrc", @"avcapture");
        check([client hasVideoDisplayRegistered],
              @"Baresip v4 registers iosview vidisp", @"iosview");

        // 4. Test VideoToolbox H.264 encode -> RFC 6184 RTP packetize -> depacketize -> decode round-trip
        BOOL h264Ok = [SipClient testH264RoundTripWithWidth:640 height:480];
        check(h264Ok,
              @"VideoToolbox H.264 encoder + RFC 6184 RTP packetizer/depacketizer + decoder round-trip reconstructs 640x480 YUV420P frame",
              @"640x480");

        // 5. Test audio & video device controls on SipClient
        [client setMuted:YES];
        check(client.isMicrophoneMuted == YES, @"SipClient toggles microphone mute ON", @"");
        [client setMuted:NO];
        check(client.isMicrophoneMuted == NO, @"SipClient toggles microphone mute OFF", @"");

        [client switchCamera];
        check(client.cameraPosition == SipCameraPositionBack, @"SipClient switches camera front -> back", @"");
        [client switchCamera];
        check(client.cameraPosition == SipCameraPositionFront, @"SipClient switches camera back -> front", @"");

        client.localVideoEnabled = NO;
        check(client.isLocalVideoEnabled == NO, @"SipClient disables local video stream", @"");
        client.localVideoEnabled = YES;
        check(client.isLocalVideoEnabled == YES, @"SipClient re-enables local video stream", @"");

        // 6. Test outgoing video call creation and SipCall controls + CallKit manager
        SipCall *videoCall = [client makeCall:@"sip:bob@127.0.0.1:5099" withVideo:YES];
        check(videoCall != nil, @"SipClient creates outgoing video SipCall", videoCall.remoteUri);
        check(videoCall.uuid != nil, @"SipCall assigns CallKit UUID", videoCall.uuid.UUIDString);
        check(videoCall.isOutgoing == YES, @"SipCall marks outgoing call", @"");
        check([videoCall.remoteUri containsString:@"bob@127.0.0.1"], @"SipCall reports remoteUri", videoCall.remoteUri);

        SipCallKitManager *callKit = [SipCallKitManager shared];
        check(callKit.isReady == YES, @"SipCallKitManager configures CXProvider with video support", @"supportsVideo=YES");
        [callKit registerCall:videoCall];
        check([callKit callFor:videoCall.uuid] == videoCall,
              @"SipCallKitManager tracks SipCall by UUID",
              videoCall.uuid.UUIDString);

        int dtmfRc = [videoCall sendDtmf:@"123#"];
        check(dtmfRc == 0, @"SipCall accepts DTMF digits", [NSString stringWithFormat:@"rc=%d", dtmfRc]);

        [videoCall hangup:487 reason:@"Request Terminated"];

        // 7. Test SipAccountStore persistence & clear
        NSUserDefaults *testDefaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.wiseland.SipSdk.unitTests"];
        [testDefaults removePersistentDomainForName:@"com.wiseland.SipSdk.unitTests"];
        SipAccountStore *accountStore = [[SipAccountStore alloc] initWithDefaults:testDefaults];
        SipAccountProfile *profile = [[SipAccountProfile alloc] initWithUsername:@"bob"
                                                                          domain:@"iptel.org"
                                                                        password:@"pw123"
                                                                     displayName:@"Bob Video"
                                                                            port:5060
                                                                       transport:@"udp"
                                                           videoEnabledByDefault:YES];
        [accountStore saveProfile:profile];
        SipAccountProfile *loaded = [accountStore loadProfile];
        check(loaded != nil && [loaded.aorDisplay isEqualToString:@"sip:bob@iptel.org"],
              @"SipAccountStore persists and loads SIP account profile",
              loaded.aorDisplay);

        // 8. Test SipCallHistoryStore recording & ordering
        SipCallHistoryStore *historyStore = [[SipCallHistoryStore alloc] initWithDefaults:testDefaults];
        [historyStore clearAll];
        [historyStore recordCallWithRemoteUri:@"sip:echo@iptel.org"
                                   isOutgoing:YES
                                     hasVideo:YES
                              durationSeconds:65
                                      outcome:@"Completed"];
        NSArray<SipCallHistoryEntry *> *entries = [historyStore entries];
        check(entries.count == 1 && [entries.firstObject.formattedSummary containsString:@"[Video] sip:echo@iptel.org (01:05)"],
              @"SipCallHistoryStore records video call entry with duration",
              entries.firstObject.formattedSummary);

        // 9. Stop stack and confirm clean restart
        [client stop];
        int restartRes = [client start];
        check(restartRes == 0, @"SipClient restarts cleanly after stop",
              [NSString stringWithFormat:@"rc=%d", restartRes]);
        [client stop];

        if (gFailures == 0) {
            printf("ALL_IOS_VIDEO_TESTS_PASSED\n");
            return 0;
        } else {
            printf("FAILED: %d test(s)\n", gFailures);
            return 1;
        }
    }
}
