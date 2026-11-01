#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CallKit/CallKit.h>
#import <AVFoundation/AVFoundation.h>
#import <arpa/inet.h>
#import <sys/socket.h>
#import <unistd.h>
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

// Minimal loopback SIP registrar: answers REGISTER with 401 Unauthorized,
// then 200 OK once the request carries an Authorization header. Exercises the
// real libre -> baresip -> SipClient response path end to end.
@interface LoopbackRegistrar : NSObject
@property (nonatomic) int sockFd;
@property (nonatomic) uint16_t port;
@property (nonatomic) BOOL running;
@end

@implementation LoopbackRegistrar

- (instancetype)initWithPort:(uint16_t)port
{
    self = [super init];
    self.port = port;
    self.sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(self.sockFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(self.sockFd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(self.sockFd);
        self.sockFd = -1;
    }
    return self;
}

- (BOOL)isBound
{
    return self.sockFd >= 0;
}

- (void)start
{
    self.running = YES;
    [NSThread detachNewThreadSelector:@selector(serveLoop) toTarget:self withObject:nil];
}

- (void)stop
{
    self.running = NO;
    if (self.sockFd >= 0) {
        shutdown(self.sockFd, SHUT_RDWR);
        close(self.sockFd);
        self.sockFd = -1;
    }
}

static NSString *sipHeader(NSString *msg, NSString *name)
{
    NSString *prefix = [name.lowercaseString stringByAppendingString:@":"];
    for (NSString *line in [msg componentsSeparatedByString:@"\r\n"]) {
        if ([line.lowercaseString hasPrefix:prefix]) {
            return [[line substringFromIndex:prefix.length]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        }
    }
    return @"";
}

- (void)serveLoop
{
    @autoreleasepool {
        char buf[8192];
        struct sockaddr_in peer;
        socklen_t peerLen = sizeof(peer);
        while (self.running) {
            ssize_t n = recvfrom(self.sockFd, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&peer, &peerLen);
            if (n <= 0) break;
            buf[n] = '\0';
            NSString *msg = [NSString stringWithUTF8String:buf];
            if (![msg hasPrefix:@"REGISTER"]) continue;

            NSString *via = sipHeader(msg, @"Via");
            NSString *from = sipHeader(msg, @"From");
            NSString *to = sipHeader(msg, @"To");
            NSString *callId = sipHeader(msg, @"Call-ID");
            NSString *cseq = sipHeader(msg, @"CSeq");
            if (![to containsString:@"tag="]) {
                to = [to stringByAppendingString:@";tag=srv42"];
            }

            NSString *status;
            NSString *extra = @"";
            if ([sipHeader(msg, @"Authorization") length] == 0) {
                status = @"401 Unauthorized";
                extra = @"WWW-Authenticate: Digest realm=\"sipreg-test\", nonce=\"deadbeef1234\", algorithm=MD5\r\n";
            } else {
                status = @"200 OK";
            }
            NSString *resp = [NSString stringWithFormat:
                @"SIP/2.0 %@\r\nVia: %@\r\nFrom: %@\r\nTo: %@\r\nCall-ID: %@\r\nCSeq: %@\r\n%@Content-Length: 0\r\n\r\n",
                status, via, from, to, callId, cseq, extra];
            sendto(self.sockFd, resp.UTF8String, [resp lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
                   0, (struct sockaddr *)&peer, peerLen);
        }
    }
}

@end

@interface LoopbackAnswerPeer : LoopbackRegistrar
@property (atomic) BOOL acknowledged;
@end

@implementation LoopbackAnswerPeer
- (void)serveLoop
{
    @autoreleasepool {
        char buf[8192];
        while (self.running) {
            struct sockaddr_in peer = {0};
            socklen_t peerLen = sizeof(peer);
            ssize_t n = recvfrom(self.sockFd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&peer, &peerLen);
            if (n <= 0) break;
            NSString *msg = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n
                                                  encoding:NSUTF8StringEncoding];
            if ([msg hasPrefix:@"ACK "]) {
                self.acknowledged = YES;
                continue;
            }
            BOOL invite = [msg hasPrefix:@"INVITE "];
            if (!invite && ![msg hasPrefix:@"BYE "]) continue;
            NSMutableString *body = [NSMutableString string];
            if (invite) {
                [body appendString:@"v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=Answer test\r\n"
                                    "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                                    "m=audio 16000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"];
                NSRegularExpression *regex = [NSRegularExpression
                    regularExpressionWithPattern:@"a=rtpmap:(\\d+) H264/90000"
                    options:NSRegularExpressionCaseInsensitive error:nil];
                NSTextCheckingResult *match = [regex firstMatchInString:msg options:0
                                                                 range:NSMakeRange(0, msg.length)];
                if (match) {
                    NSString *pt = [msg substringWithRange:[match rangeAtIndex:1]];
                    [body appendFormat:@"m=video 16002 RTP/AVP %@\r\na=rtpmap:%@ H264/90000\r\n"
                                        "a=fmtp:%@ packetization-mode=1;profile-level-id=42e01f\r\na=sendrecv\r\n",
                                        pt, pt, pt];
                }
            }
            NSString *to = sipHeader(msg, @"To");
            if (![to containsString:@";tag="]) to = [to stringByAppendingString:@";tag=answer-test"];
            NSString *response = [NSString stringWithFormat:
                @"SIP/2.0 200 OK\r\nVia: %@\r\nFrom: %@\r\nTo: %@\r\nCall-ID: %@\r\nCSeq: %@\r\n"
                 "Contact: <sip:peer@127.0.0.1:15099>\r\nContent-Type: application/sdp\r\n"
                 "Content-Length: %lu\r\n\r\n%@",
                sipHeader(msg, @"Via"), sipHeader(msg, @"From"), to,
                sipHeader(msg, @"Call-ID"), sipHeader(msg, @"CSeq"),
                (unsigned long)[body lengthOfBytesUsingEncoding:NSUTF8StringEncoding], body];
            sendto(self.sockFd, response.UTF8String,
                   [response lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
                   0, (struct sockaddr *)&peer, peerLen);
        }
    }
}
@end

@interface LoopbackReinvitePeer : LoopbackRegistrar
@property (atomic) BOOL acknowledged;
@property (atomic) uint16_t clientVideoPort;
@property (atomic) BOOL reinviteAnswered;
@property (nonatomic) NSString *lastCallId;
@property (nonatomic) NSString *clientContact;
@property (nonatomic) NSString *clientFrom;
@property (nonatomic) NSString *peerTo;
@property (nonatomic) struct sockaddr_in clientSockAddr;
@property (nonatomic) socklen_t clientSockLen;
- (void)sendReinviteRecvonly;
@end

@implementation LoopbackReinvitePeer
- (void)serveLoop
{
    @autoreleasepool {
        char buf[8192];
        while (self.running) {
            struct sockaddr_in peer = {0};
            socklen_t peerLen = sizeof(peer);
            ssize_t n = recvfrom(self.sockFd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&peer, &peerLen);
            if (n <= 0) break;
            NSString *msg = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n
                                                   encoding:NSUTF8StringEncoding];
            if ([msg hasPrefix:@"ACK "]) {
                self.acknowledged = YES;
                continue;
            }
            if ([msg hasPrefix:@"SIP/2.0 200 OK"]) {
                NSString *cseq = sipHeader(msg, @"CSeq");
                if ([cseq containsString:@"100 INVITE"]) {
                    self.reinviteAnswered = YES;
                    NSString *ack = [NSString stringWithFormat:
                        @"ACK %@ SIP/2.0\r\n"
                        "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-ack-reinvite;rport\r\n"
                        "Max-Forwards: 70\r\n"
                        "From: %@\r\n"
                        "To: %@\r\n"
                        "Call-ID: %@\r\n"
                        "CSeq: 100 ACK\r\n"
                        "Content-Length: 0\r\n\r\n",
                        self.clientContact ?: @"sip:alice@127.0.0.1",
                        self.port,
                        self.peerTo,
                        self.clientFrom,
                        self.lastCallId];
                    sendto(self.sockFd, ack.UTF8String,
                           [ack lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
                           0, (struct sockaddr *)&peer, peerLen);
                }
                continue;
            }
            BOOL invite = [msg hasPrefix:@"INVITE "];
            if (!invite && ![msg hasPrefix:@"BYE "]) continue;

            if (invite) {
                self.clientSockAddr = peer;
                self.clientSockLen = peerLen;
                self.lastCallId = sipHeader(msg, @"Call-ID");
                self.clientContact = sipHeader(msg, @"Contact");
                if ([self.clientContact hasPrefix:@"<"] && [self.clientContact hasSuffix:@">"]) {
                    self.clientContact = [self.clientContact substringWithRange:NSMakeRange(1, self.clientContact.length - 2)];
                }
                self.clientFrom = sipHeader(msg, @"From");

                NSRegularExpression *vRegex = [NSRegularExpression
                    regularExpressionWithPattern:@"m=video (\\d+) RTP/AVP"
                    options:NSRegularExpressionCaseInsensitive error:nil];
                NSTextCheckingResult *vMatch = [vRegex firstMatchInString:msg options:0 range:NSMakeRange(0, msg.length)];
                if (vMatch) {
                    self.clientVideoPort = (uint16_t)[[msg substringWithRange:[vMatch rangeAtIndex:1]] intValue];
                }
            }

            NSMutableString *body = [NSMutableString string];
            if (invite) {
                [body appendString:@"v=0\r\no=peer 1 1 IN IP4 127.0.0.1\r\ns=Answer test\r\n"
                                    "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                                    "m=audio 16000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"];
                NSRegularExpression *regex = [NSRegularExpression
                    regularExpressionWithPattern:@"a=rtpmap:(\\d+) H264/90000"
                    options:NSRegularExpressionCaseInsensitive error:nil];
                NSTextCheckingResult *match = [regex firstMatchInString:msg options:0
                                                                 range:NSMakeRange(0, msg.length)];
                if (match) {
                    NSString *pt = [msg substringWithRange:[match rangeAtIndex:1]];
                    [body appendFormat:@"m=video 16002 RTP/AVP %@\r\na=rtpmap:%@ H264/90000\r\n"
                                        "a=fmtp:%@ packetization-mode=1;profile-level-id=42e01f\r\na=sendrecv\r\n",
                                        pt, pt, pt];
                }
            }
            NSString *to = sipHeader(msg, @"To");
            if (![to containsString:@";tag="]) to = [to stringByAppendingString:@";tag=reinvite-peer"];
            self.peerTo = to;
            NSString *response = [NSString stringWithFormat:
                @"SIP/2.0 200 OK\r\nVia: %@\r\nFrom: %@\r\nTo: %@\r\nCall-ID: %@\r\nCSeq: %@\r\n"
                 "Contact: <sip:peer@127.0.0.1:%u>\r\nContent-Type: application/sdp\r\n"
                 "Content-Length: %lu\r\n\r\n%@",
                sipHeader(msg, @"Via"), sipHeader(msg, @"From"), to,
                sipHeader(msg, @"Call-ID"), sipHeader(msg, @"CSeq"),
                self.port,
                (unsigned long)[body lengthOfBytesUsingEncoding:NSUTF8StringEncoding], body];
            sendto(self.sockFd, response.UTF8String,
                   [response lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
                   0, (struct sockaddr *)&peer, peerLen);
        }
    }
}

- (void)sendReinviteRecvonly
{
    NSString *sdp = [NSString stringWithFormat:
        @"v=0\r\no=peer 1 2 IN IP4 127.0.0.1\r\ns=Reinvite recvonly test\r\n"
        "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
        "m=audio 16000 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=sendrecv\r\n"
        "m=video 16002 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=recvonly\r\n"];

    NSString *reinvite = [NSString stringWithFormat:
        @"INVITE %@ SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-reinvite-100;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %@\r\n"
        "To: %@\r\n"
        "Call-ID: %@\r\n"
        "CSeq: 100 INVITE\r\n"
        "Contact: <sip:peer@127.0.0.1:%u>\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %lu\r\n\r\n%@",
        self.clientContact ?: @"sip:alice@127.0.0.1",
        self.port,
        self.peerTo,
        self.clientFrom,
        self.lastCallId,
        self.port,
        (unsigned long)[sdp lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
        sdp];

    sendto(self.sockFd, reinvite.UTF8String,
           [reinvite lengthOfBytesUsingEncoding:NSUTF8StringEncoding],
           0, (struct sockaddr *)&_clientSockAddr, _clientSockLen);
}
@end

@interface AnswerProbe : NSObject <SipClientDelegate>
@property (nonatomic) BOOL established;
@property (nonatomic) BOOL closed;
@end

@implementation AnswerProbe
- (void)onCallEstablished:(SipCall *)call { self.established = YES; }
- (void)onCallClosed:(SipCall *)call { self.closed = YES; }
@end

static void testAnsweredCall(SipClient *client, BOOL withVideo)
{
    AVAudioSession *session = AVAudioSession.sharedInstance;
    [session setActive:NO error:nil];
    [session setCategory:AVAudioSessionCategorySoloAmbient error:nil];
    LoopbackAnswerPeer *peer = [[LoopbackAnswerPeer alloc] initWithPort:15099];
    check(peer.isBound, @"Answering SIP peer binds loopback port", @"15099");
    if (!peer.isBound) return;
    [peer start];
    AnswerProbe *probe = [AnswerProbe new];
    client.delegate = probe;
    SipCall *call = [client makeCall:@"sip:peer@127.0.0.1:15099" withVideo:withVideo];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5];
    while (!probe.closed && !probe.established && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    NSDate *connectedDeadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (probe.established && !probe.closed && connectedDeadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    check(call != nil && probe.established && !probe.closed && peer.acknowledged &&
          call.hasVideo == withVideo,
          withVideo ? @"Answered video call stays connected" : @"Answered audio call stays connected",
          [NSString stringWithFormat:@"established=%d closed=%d ack=%d",
           probe.established, probe.closed, peer.acknowledged]);
    [call hangup:0 reason:@"Test complete"];
    NSDate *closeDeadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!probe.closed && closeDeadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    [peer stop];
    client.delegate = nil;
}

static void testReinviteVideoCall(SipClient *client)
{
    LoopbackReinvitePeer *peer = [[LoopbackReinvitePeer alloc] initWithPort:15098];
    check(peer.isBound, @"Re-INVITE loopback peer binds port 15098", @"15098");
    if (!peer.isBound) return;
    [peer start];

    AnswerProbe *probe = [AnswerProbe new];
    client.delegate = probe;
    SipVideoView *remoteView = [[SipVideoView alloc] initWithFrame:CGRectMake(0, 0, 320, 240)];
    client.remoteVideoView = remoteView;

    SipCall *call = [client makeCall:@"sip:peer@127.0.0.1:15098" withVideo:YES];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (!probe.closed && !probe.established && deadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    check(call != nil && probe.established && !probe.closed && peer.acknowledged && peer.clientVideoPort > 0,
          @"Video call establishes and client advertises video port",
          [NSString stringWithFormat:@"est=%d port=%u", probe.established, peer.clientVideoPort]);

    // Send 2 video frames to the client before re-INVITE
    uint16_t seq = 1;
    uint32_t ts = 90000;
    uint32_t ssrc = 0x11223344;
    BOOL sent1 = [SipClient sendH264TestFrameToPort:peer.clientVideoPort ssrc:ssrc seq:&seq ts:ts width:640 height:480 idr:YES];
    ts += 3000;
    BOOL sent2 = [SipClient sendH264TestFrameToPort:peer.clientVideoPort ssrc:ssrc seq:&seq ts:ts width:640 height:480 idr:NO];
    check(sent1 && sent2, @"Sent test H.264 video frames before re-INVITE", @"");

    // Wait for initial frames before re-INVITE to fully render
    NSDate *renderDeadline1 = [NSDate dateWithTimeIntervalSinceNow:1.5];
    while (remoteView.renderedFrameCount < 2 && renderDeadline1.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    NSUInteger framesBefore = remoteView.renderedFrameCount;
    check(framesBefore >= 2, @"Remote video view receives and renders frames before re-INVITE",
          [NSString stringWithFormat:@"count=%lu", (unsigned long)framesBefore]);

    // Peer sends mid-call re-INVITE with a=recvonly (simulating Linphone / Asterisk mid-call re-INVITE)
    [peer sendReinviteRecvonly];
    NSDate *reinviteDeadline = [NSDate dateWithTimeIntervalSinceNow:2.0];
    while (!peer.reinviteAnswered && reinviteDeadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    check(peer.reinviteAnswered, @"Client answers mid-call re-INVITE with a=recvonly", @"200 OK");

    // Allow re-INVITE transaction and media update to fully take effect
    NSDate *settleDeadline = [NSDate dateWithTimeIntervalSinceNow:0.3];
    while (settleDeadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    NSUInteger framesAtReinvite = remoteView.renderedFrameCount;

    // Send 2 more video frames AFTER re-INVITE (like Linphone streaming video through Asterisk)
    ts += 3000;
    [SipClient sendH264TestFrameToPort:peer.clientVideoPort ssrc:ssrc seq:&seq ts:ts width:640 height:480 idr:YES];
    ts += 3000;
    [SipClient sendH264TestFrameToPort:peer.clientVideoPort ssrc:ssrc seq:&seq ts:ts width:640 height:480 idr:NO];

    NSDate *renderDeadline2 = [NSDate dateWithTimeIntervalSinceNow:1.5];
    while (remoteView.renderedFrameCount == framesAtReinvite && renderDeadline2.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    NSUInteger framesAfter = remoteView.renderedFrameCount;
    check(framesAfter > framesAtReinvite,
          @"Remote video view continues to render video frames after mid-call re-INVITE (no freeze)",
          [NSString stringWithFormat:@"atReinvite=%lu after=%lu", (unsigned long)framesAtReinvite, (unsigned long)framesAfter]);

    [call hangup:0 reason:@"Test complete"];
    NSDate *closeDeadline = [NSDate dateWithTimeIntervalSinceNow:2.0];
    while (!probe.closed && closeDeadline.timeIntervalSinceNow > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    [peer stop];
    client.delegate = nil;
}

@interface RegisterProbe : NSObject <SipClientDelegate>
@property (nonatomic) BOOL registered;
@property (nonatomic) BOOL failed;
@end

@implementation RegisterProbe
- (void)onDidRegister:(SipClient *)sipSdk { self.registered = YES; }
- (void)onFailedRegister:(SipClient *)sipSdk { self.failed = YES; }
@end

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

        // 3b. Register against a loopback registrar: REGISTER -> 401 -> auth REGISTER -> 200 OK.
        // Locks the full response path (socket -> transaction -> sipreg -> bevent -> delegate).
        LoopbackRegistrar *registrar = [[LoopbackRegistrar alloc] initWithPort:(uint16_t)client.port];
        check(registrar.isBound, @"Loopback SIP registrar binds 127.0.0.1:5060", @"");
        [registrar start];
        [NSThread sleepForTimeInterval:0.2];

        RegisterProbe *probe = [[RegisterProbe alloc] init];
        client.delegate = probe;

        int regRc = [client registry];
        check(regRc == 0, @"SipClient registry() submits REGISTER",
              [NSString stringWithFormat:@"rc=%d", regRc]);

        NSDate *regDeadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
        while (!probe.registered && !probe.failed &&
               [[NSDate date] compare:regDeadline] == NSOrderedAscending) {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        check(probe.registered,
              @"REGISTER round-trip yields REGISTER_OK delegate callback",
              [NSString stringWithFormat:@"registered=%d failed=%d", probe.registered, probe.failed]);
        check(client.isRegistered,
              @"SipClient reports registered state after 200 OK", @"");
        [registrar stop];
        client.delegate = nil;

        testAnsweredCall(client, NO);
        testAnsweredCall(client, YES);
        testReinviteVideoCall(client);

        // 4. Test VideoToolbox H.264 encode -> RFC 6184 RTP packetize -> depacketize -> decode round-trip
        BOOL h264Ok = [SipClient testH264RoundTripWithWidth:640 height:480];
        check(h264Ok,
              @"VideoToolbox H.264 encoder + RFC 6184 RTP packetizer/depacketizer + decoder round-trip reconstructs 640x480 YUV420P frame",
              @"640x480");

        // 4b. Packet loss in the remote H.264 stream must not freeze video forever
        BOOL lossOk = [SipClient testH264PacketLossRecoveryWithWidth:640 height:480];
        check(lossOk,
              @"H.264 decoder reports RTP packet loss, hides broken frames, and recovers on next IDR",
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

        // 5b. Test standalone local video preview outside of a call
        NSUInteger framesBeforePreview = localView.renderedFrameCount;
        [client startLocalVideoPreview];
        check(client.isLocalVideoPreviewActive == YES,
              @"SipClient starts local video preview outside a call", @"");
        [NSThread sleepForTimeInterval:0.5];
        check(localView.renderedFrameCount > framesBeforePreview,
              @"Local preview feeds frames into localVideoView without an active call",
              [NSString stringWithFormat:@"frames=%lu liveCamera=%d",
               (unsigned long)localView.renderedFrameCount,
               client.isLiveCameraFeedActive ? 1 : 0]);
        [client stopLocalVideoPreview];
        check(client.isLocalVideoPreviewActive == NO, @"SipClient stops local video preview", @"");

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
