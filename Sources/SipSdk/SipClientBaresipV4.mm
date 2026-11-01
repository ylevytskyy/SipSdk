//
// SipClientBaresipV4.mm
// Modern Baresip v4.12.0 Objective-C++ adapter with native iOS VideoToolbox H.264
// codec (vidcodec), AVCaptureSession + Simulator synthetic video source (vidsrc),
// SipVideoView renderer (vidisp), and full telephony controls.
//

#import "SipClient.h"
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>
#import <BaresipCore.h>
#import <rem.h>

#include <errno.h>
#include <string.h>
#include <vector>
#include <mutex>

static const int kDefaultPort = 5060;
static const unsigned kAsyncWorkerCount = 4;
static const size_t kDefaultRtpPayloadSize = 1200;

// ============================================================================
// MARK: - SipVideoView Implementation
// ============================================================================

@interface SipVideoView ()
@property (nonatomic, readwrite) CGSize videoSize;
@property (nonatomic, readwrite) NSUInteger renderedFrameCount;
@end

@implementation SipVideoView {
    uint32_t _patternTick;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder
{
    self = [super initWithCoder:coder];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (void)commonInit
{
    self.backgroundColor = [UIColor colorWithRed:0.07 green:0.08 blue:0.11 alpha:1.0];
    self.clipsToBounds = YES;
    self.layer.contentsGravity = kCAGravityResizeAspectFill;
    _videoSize = CGSizeZero;
    _renderedFrameCount = 0;
    _patternTick = 0;
}

static inline uint8_t clampByte(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

- (void)renderYUV420WithY:(const uint8_t *)yPlane
                        u:(const uint8_t *)uPlane
                        v:(const uint8_t *)vPlane
                  yStride:(int)yStride
                  uStride:(int)uStride
                  vStride:(int)vStride
                    width:(int)width
                   height:(int)height
{
    if (!yPlane || !uPlane || !vPlane || width <= 0 || height <= 0) {
        return;
    }

    size_t rgbaLen = (size_t)width * (size_t)height * 4;
    uint8_t *rgba = (uint8_t *)malloc(rgbaLen);
    if (!rgba) {
        return;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *yRow = yPlane + y * yStride;
        const uint8_t *uRow = uPlane + (y / 2) * uStride;
        const uint8_t *vRow = vPlane + (y / 2) * vStride;
        uint8_t *dstRow = rgba + (size_t)y * (size_t)width * 4;

        for (int x = 0; x < width; x++) {
            int Y = (int)yRow[x] - 16;
            int U = (int)uRow[x / 2] - 128;
            int V = (int)vRow[x / 2] - 128;
            if (Y < 0) Y = 0;

            int c = 298 * Y;
            int r = (c + 409 * V + 128) >> 8;
            int g = (c - 100 * U - 208 * V + 128) >> 8;
            int b = (c + 516 * U + 128) >> 8;

            dstRow[x * 4 + 0] = clampByte(r);
            dstRow[x * 4 + 1] = clampByte(g);
            dstRow[x * 4 + 2] = clampByte(b);
            dstRow[x * 4 + 3] = 255;
        }
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, rgba, rgbaLen,
        [](void *info, const void *data, size_t size) {
            free((void *)data);
        });
    CGImageRef cgImage = CGImageCreate(
        (size_t)width, (size_t)height,
        8, 32, (size_t)width * 4,
        colorSpace,
        kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast,
        provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);

    if (!cgImage) {
        return;
    }

    self.videoSize = CGSizeMake(width, height);
    self.renderedFrameCount += 1;

    if ([NSThread isMainThread]) {
        self.layer.contents = (__bridge id)cgImage;
        CGImageRelease(cgImage);
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.layer.contents = (__bridge id)cgImage;
            CGImageRelease(cgImage);
        });
    }
}

static void fillSyntheticYUV420Pattern(uint8_t *yPlane,
                                       uint8_t *uPlane,
                                       uint8_t *vPlane,
                                       int width,
                                       int height,
                                       uint32_t tick)
{
    // SMPTE-inspired color bars in YUV + moving bar indicator
    static const uint8_t kBarY[8] = {235, 210, 170, 145, 106,  81,  41,  24};
    static const uint8_t kBarU[8] = {128,  16, 166,  54, 202,  90, 240, 128};
    static const uint8_t kBarV[8] = {128, 146,  16,  34, 222, 240, 110, 128};

    int barWidth = width / 8;
    if (barWidth < 1) barWidth = 1;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int barIdx = (x / barWidth) & 7;
            yPlane[y * width + x] = kBarY[barIdx];
        }
    }

    int uvW = width / 2;
    int uvH = height / 2;
    for (int y = 0; y < uvH; y++) {
        for (int x = 0; x < uvW; x++) {
            int barIdx = ((x * 2) / barWidth) & 7;
            uPlane[y * uvW + x] = kBarU[barIdx];
            vPlane[y * uvW + x] = kBarV[barIdx];
        }
    }

    // Animated white/cyan scan block so live motion is visibly verifiable
    int boxW = width / 8;
    int boxH = height / 8;
    int boxX = (int)((tick * 6) % (unsigned)(width - boxW));
    int boxY = height / 2 - boxH / 2;

    for (int y = boxY; y < boxY + boxH && y < height; y++) {
        if (y < 0) continue;
        for (int x = boxX; x < boxX + boxW && x < width; x++) {
            yPlane[y * width + x] = 235;
            uPlane[(y / 2) * uvW + (x / 2)] = 180;
            vPlane[(y / 2) * uvW + (x / 2)] = 80;
        }
    }
}

- (void)renderTestPatternWithWidth:(int)width height:(int)height
{
    if (width <= 0 || height <= 0) {
        return;
    }
    int uvW = width / 2;
    int uvH = height / 2;
    std::vector<uint8_t> yPlane((size_t)width * (size_t)height);
    std::vector<uint8_t> uPlane((size_t)uvW * (size_t)uvH);
    std::vector<uint8_t> vPlane((size_t)uvW * (size_t)uvH);

    fillSyntheticYUV420Pattern(yPlane.data(), uPlane.data(), vPlane.data(),
                               width, height, ++_patternTick);
    [self renderYUV420WithY:yPlane.data()
                          u:uPlane.data()
                          v:vPlane.data()
                    yStride:width
                    uStride:uvW
                    vStride:uvW
                      width:width
                     height:height];
}

- (void)clearVideo
{
    self.videoSize = CGSizeZero;
    if ([NSThread isMainThread]) {
        self.layer.contents = nil;
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.layer.contents = nil;
        });
    }
}

@end

// ============================================================================
// MARK: - IosVideoRendererDriver Implementation
// ============================================================================

static void ios_renderer_render_frame(void *user_data, const sip_video_frame_t *frame)
{
    if (!user_data || !frame) return;
    IosVideoRendererDriver *driver = (__bridge IosVideoRendererDriver *)user_data;
    [driver renderFrame:frame];
}

static void ios_renderer_on_stream_resized(void *user_data, int width, int height)
{
    if (!user_data) return;
    IosVideoRendererDriver *driver = (__bridge IosVideoRendererDriver *)user_data;
    [driver onStreamResizedWithWidth:width height:height];
}

static void ios_renderer_clear(void *user_data)
{
    if (!user_data) return;
    IosVideoRendererDriver *driver = (__bridge IosVideoRendererDriver *)user_data;
    [driver clear];
}

@implementation IosVideoRendererDriver

- (instancetype)initWithVideoView:(nullable SipVideoView *)videoView
{
    self = [super init];
    if (self) {
        _videoView = videoView;
    }
    return self;
}

- (sip_video_renderer_driver_t)cDriver
{
    sip_video_renderer_driver_t driver{};
    driver.user_data = (__bridge void *)self;
    driver.render_frame = ios_renderer_render_frame;
    driver.on_stream_resized = ios_renderer_on_stream_resized;
    driver.clear = ios_renderer_clear;
    return driver;
}

- (void)renderFrame:(const sip_video_frame_t *)frame
{
    if (!frame) return;
    SipVideoView *view = self.videoView;
    if (view == nil) return;

    [view renderYUV420WithY:frame->y
                          u:frame->u
                          v:frame->v
                    yStride:frame->y_stride
                    uStride:frame->u_stride
                    vStride:frame->v_stride
                      width:frame->width
                     height:frame->height];
}

- (void)onStreamResizedWithWidth:(int)width height:(int)height
{
    SipVideoView *view = self.videoView;
    if (view == nil || width <= 0 || height <= 0) return;

    void (^updateBlock)(void) = ^{
        view.videoSize = CGSizeMake(width, height);
    };
    if ([NSThread isMainThread]) {
        updateBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), updateBlock);
    }
}

- (void)clear
{
    SipVideoView *view = self.videoView;
    if (view == nil) return;

    void (^clearBlock)(void) = ^{
        [view clearVideo];
    };
    if ([NSThread isMainThread]) {
        clearBlock();
    } else {
        dispatch_async(dispatch_get_main_queue(), clearBlock);
    }
}

@end

// ============================================================================
// MARK: - Private Interfaces & Active Client State
// ============================================================================

@interface SipCall ()
@property (nonatomic, assign) sip_call_t *coreCall;
@property (nonatomic) struct ua *ua;
@property (nonatomic) struct call *call;
@property (nonatomic, copy) NSString *cachedRemoteUri;
@property (nonatomic, copy) NSString *cachedCallId;
@property (nonatomic, strong) NSUUID *callUuid;
@property (nonatomic, assign) BOOL callHasVideo;
@property (nonatomic, assign) BOOL callIsOutgoing;
@property (nonatomic, assign) BOOL callOnHold;
@property (nonatomic, assign) BOOL closed;
@property (nonatomic, assign) uint32_t cachedDuration;
@property (nonatomic, strong) NSDate *establishedAt;
@end

@class SipAVCaptureHelper;

@interface SipClient ()
@property (nonatomic, assign) sip_client_t *coreClient;
@property (nonatomic, strong) IosVideoCaptureDriver *captureDriver;
@property (nonatomic, strong) IosVideoRendererDriver *remoteRendererDriver;
@property (nonatomic, strong) IosVideoRendererDriver *localRendererDriver;
@property (nonatomic, strong) IosVideoCodecDriver *codecDriver;
@property (nonatomic) struct ua *ua;
@property (nonatomic, strong, nullable) SipAVCaptureHelper *localPreviewHelper;
@property (nonatomic) struct mqueue *controlMq;
@property (nonatomic, strong) NSThread *loopThread;
@property (nonatomic, strong) dispatch_semaphore_t loopStartedSemaphore;
@property (nonatomic, strong) dispatch_semaphore_t loopStoppedSemaphore;
@property (nonatomic) int startResult;
@property (nonatomic) BOOL loopThreadStarted;
@property (nonatomic) BOOL stackStarted;
@property (nonatomic) BOOL eventsRegistered;
@property (nonatomic) BOOL mediaPluginsRegistered;
@property (nonatomic) BOOL callAudioSessionActive;
- (BOOL)prepareCallAudioSession;
- (void)releaseCallAudioSessionIfIdle;
@end

@interface SipLoopTask : NSObject
@property (nonatomic, copy) void (^block)(void);
@property (nonatomic, strong) dispatch_semaphore_t doneSemaphore;
@end

@implementation SipLoopTask
@end

static void control_mqueue_handler(int id, void *data, void *arg)
{
    (void)arg;
    if (id == 1) {
        re_cancel();
    } else if (id == 2 && data != nullptr) {
        SipLoopTask *task = (__bridge_transfer SipLoopTask *)data;
        if (task.block) {
            task.block();
        }
        if (task.doneSemaphore) {
            dispatch_semaphore_signal(task.doneSemaphore);
        }
    }
}

static void performAsyncOnSipLoop(SipClient *client, void (^block)(void))
{
    if (!block) {
        return;
    }
    if (client == nil || !client.stackStarted || client.controlMq == nullptr || [NSThread currentThread] == client.loopThread) {
        block();
        return;
    }
    SipLoopTask *task = [[SipLoopTask alloc] init];
    task.block = block;
    task.doneSemaphore = nil;
    if (mqueue_push(client.controlMq, 2, (__bridge_retained void *)task) != 0) {
        block();
    }
}

static void performSyncOnSipLoop(SipClient *client, void (^block)(void))
{
    if (!block) {
        return;
    }
    if (client == nil || !client.stackStarted || client.controlMq == nullptr || [NSThread currentThread] == client.loopThread) {
        block();
        return;
    }
    SipLoopTask *task = [[SipLoopTask alloc] init];
    task.block = block;
    task.doneSemaphore = dispatch_semaphore_create(0);
    if (mqueue_push(client.controlMq, 2, (__bridge_retained void *)task) == 0) {
        dispatch_semaphore_wait(task.doneSemaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)));
    } else {
        block();
    }
}

static __weak SipClient *gActiveSipClient = nil;

// ============================================================================
// MARK: - Hardware VideoToolbox H.264 Codec (vidcodec)
// ============================================================================

struct EncodedRtpPacket {
    bool marker;
    uint64_t rtp_ts;
    std::vector<uint8_t> hdr;
    std::vector<uint8_t> pld;
};

struct videnc_state {
    VTCompressionSessionRef session;
    unsigned width;
    unsigned height;
    struct videnc_param prm;
    videnc_packet_h *pkth;
    const struct video *vid;
    uint64_t current_rtp_ts;
    uint32_t frame_index;
    std::mutex pkt_mutex;
    std::vector<EncodedRtpPacket> pending_packets;
};

struct viddec_state {
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef formatDesc;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> format_sps;
    std::vector<uint8_t> format_pps;
    std::vector<uint8_t> au_buffer;
    std::vector<uint8_t> fu_buffer;
    bool fu_active;
    bool au_is_idr;
    std::mutex decode_mutex;
    std::vector<uint8_t> decoded_yuv;
    unsigned decoded_w;
    unsigned decoded_h;
    bool has_decoded_frame;
    // Packet-loss recovery. A lost RTP packet breaks the H.264 reference
    // chain; VideoToolbox then shows a frozen picture until the next IDR.
    // Peers like Linphone only send an IDR when asked (RTCP PLI/FIR), and
    // Baresip asks only when dech returns an error.
    bool have_seq;
    uint16_t last_seq;
    bool have_au_ts;
    uint32_t au_ts;
    bool au_corrupt;
    bool need_idr;
    bool decode_failed;
    uint64_t last_picup_ms;
};

static void sendRfc6184Nal(struct videnc_state *ves,
                           const uint8_t *nal,
                           size_t nalLen,
                           bool isLastNalOfAccessUnit)
{
    if (!ves || !ves->pkth || !nal || nalLen == 0) {
        return;
    }

    size_t maxPkt = ves->prm.pktsize >= 256 ? ves->prm.pktsize : kDefaultRtpPayloadSize;
    if (nalLen <= maxPkt) {
        EncodedRtpPacket pkt;
        pkt.marker = isLastNalOfAccessUnit;
        pkt.rtp_ts = ves->current_rtp_ts;
        pkt.hdr.assign(nal, nal + 1);
        pkt.pld.assign(nal + 1, nal + nalLen);
        std::lock_guard<std::mutex> lock(ves->pkt_mutex);
        ves->pending_packets.push_back(std::move(pkt));
        return;
    }

    // Fragmentation Unit A (RFC 6184 Section 5.8)
    uint8_t nalHeader = nal[0];
    uint8_t fuIndicator = (nalHeader & 0xE0) | H264_NALU_FU_A;
    uint8_t nalType = nalHeader & 0x1F;

    const uint8_t *payload = nal + 1;
    size_t remaining = nalLen - 1;
    size_t maxChunk = maxPkt - 2;
    bool firstChunk = true;

    while (remaining > 0) {
        size_t chunkLen = remaining > maxChunk ? maxChunk : remaining;
        bool lastChunk = (remaining == chunkLen);

        uint8_t fuHeader = nalType;
        if (firstChunk) fuHeader |= 0x80;
        if (lastChunk)  fuHeader |= 0x40;

        uint8_t hdr[2] = { fuIndicator, fuHeader };
        EncodedRtpPacket pkt;
        pkt.marker = isLastNalOfAccessUnit && lastChunk;
        pkt.rtp_ts = ves->current_rtp_ts;
        pkt.hdr.assign(hdr, hdr + 2);
        pkt.pld.assign(payload, payload + chunkLen);
        {
            std::lock_guard<std::mutex> lock(ves->pkt_mutex);
            ves->pending_packets.push_back(std::move(pkt));
        }

        payload += chunkLen;
        remaining -= chunkLen;
        firstChunk = false;
    }
}

static void vtCompressionOutputCallback(void *outputCallbackRefCon,
                                        void *sourceFrameRefCon,
                                        OSStatus status,
                                        VTEncodeInfoFlags infoFlags,
                                        CMSampleBufferRef sampleBuffer)
{
    if (status != noErr || !sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer)) {
        return;
    }

    auto *ves = static_cast<struct videnc_state *>(outputCallbackRefCon);
    if (!ves || !ves->pkth) {
        return;
    }

    bool isKeyframe = true;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        if (dict && CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync)) {
            isKeyframe = false;
        }
    }

    if (isKeyframe) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmt) {
            size_t paramCount = 0;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fmt, 0, nullptr, nullptr, &paramCount, nullptr);
            for (size_t i = 0; i < paramCount; i++) {
                const uint8_t *paramSet = nullptr;
                size_t paramLen = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                        fmt, i, &paramSet, &paramLen, nullptr, nullptr) == noErr &&
                    paramSet && paramLen > 0) {
                    sendRfc6184Nal(ves, paramSet, paramLen, false);
                }
            }
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!block) {
        return;
    }

    size_t totalLen = 0;
    char *dataPtr = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &totalLen, &dataPtr) != noErr || !dataPtr) {
        return;
    }

    size_t offset = 0;
    while (offset + 4 <= totalLen) {
        uint32_t nalLen = 0;
        memcpy(&nalLen, dataPtr + offset, 4);
        nalLen = CFSwapInt32BigToHost(nalLen);
        offset += 4;
        if (nalLen == 0 || offset + nalLen > totalLen) {
            break;
        }
        bool isLast = (offset + nalLen >= totalLen);
        sendRfc6184Nal(ves, (const uint8_t *)(dataPtr + offset), nalLen, isLast);
        offset += nalLen;
    }
}

static void h264_enc_destructor(void *arg)
{
    auto *ves = static_cast<struct videnc_state *>(arg);
    if (ves->session) {
        VTCompressionSessionInvalidate(ves->session);
        CFRelease(ves->session);
        ves->session = nullptr;
    }
    ves->~videnc_state();
}

static int h264_enc_update(struct videnc_state **vesp,
                           const struct vidcodec *vc,
                           struct videnc_param *prm,
                           const char *fmtp,
                           videnc_packet_h *pkth,
                           const struct video *vid)
{
    (void)vc;
    (void)fmtp;
    if (!vesp || !prm || !pkth) {
        return EINVAL;
    }

    struct videnc_state *ves = *vesp;
    if (!ves) {
        void *raw = mem_zalloc(sizeof(struct videnc_state), h264_enc_destructor);
        if (!raw) {
            return ENOMEM;
        }
        ves = new (raw) struct videnc_state();
        ves->session = nullptr;
        ves->width = 0;
        ves->height = 0;
        ves->current_rtp_ts = 0;
        ves->frame_index = 0;
        *vesp = ves;
    }

    ves->prm = *prm;
    ves->pkth = pkth;
    ves->vid = vid;
    return 0;
}

static int ensureEncoderSession(struct videnc_state *ves, unsigned width, unsigned height)
{
    if (ves->session && ves->width == width && ves->height == height) {
        return 0;
    }
    if (ves->session) {
        VTCompressionSessionInvalidate(ves->session);
        CFRelease(ves->session);
        ves->session = nullptr;
    }

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)width,
        (int32_t)height,
        kCMVideoCodecType_H264,
        nullptr,
        nullptr,
        kCFAllocatorDefault,
        vtCompressionOutputCallback,
        ves,
        &ves->session);
    if (status != noErr || !ves->session) {
        return EIO;
    }

    VTSessionSetProperty(ves->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(ves->session,
                         kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_Baseline_AutoLevel);
    VTSessionSetProperty(ves->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    int maxKeyframeInterval = 30;
    CFNumberRef kfNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxKeyframeInterval);
    VTSessionSetProperty(ves->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfNum);
    CFRelease(kfNum);

    int bitrate = (int)(ves->prm.bitrate > 0 ? ves->prm.bitrate : 512000);
    CFNumberRef brNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bitrate);
    VTSessionSetProperty(ves->session, kVTCompressionPropertyKey_AverageBitRate, brNum);
    CFRelease(brNum);

    VTCompressionSessionPrepareToEncodeFrames(ves->session);
    ves->width = width;
    ves->height = height;
    return 0;
}

static int h264_enc_encode(struct videnc_state *ves,
                           bool update,
                           const struct vidframe *frame,
                           uint64_t timestamp)
{
    if (!ves || !frame || !vidframe_isvalid(frame)) {
        return EINVAL;
    }

    unsigned width = frame->size.w;
    unsigned height = frame->size.h;
    if (width == 0 || height == 0) {
        return EINVAL;
    }

    int err = ensureEncoderSession(ves, width, height);
    if (err != 0) {
        return err;
    }

    CVPixelBufferRef pixelBuffer = nullptr;
    CVReturn cvErr = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        nullptr,
        &pixelBuffer);
    if (cvErr != kCVReturnSuccess || !pixelBuffer) {
        return EIO;
    }

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    uint8_t *dstY = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
    size_t dstYStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    uint8_t *dstUV = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
    size_t dstUVStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

    if (frame->fmt == VID_FMT_YUV420P) {
        for (unsigned y = 0; y < height; y++) {
            memcpy(dstY + y * dstYStride,
                   frame->data[0] + y * frame->linesize[0],
                   width);
        }
        unsigned uvW = width / 2;
        unsigned uvH = height / 2;
        for (unsigned y = 0; y < uvH; y++) {
            const uint8_t *srcU = frame->data[1] + y * frame->linesize[1];
            const uint8_t *srcV = frame->data[2] + y * frame->linesize[2];
            uint8_t *rowUV = dstUV + y * dstUVStride;
            for (unsigned x = 0; x < uvW; x++) {
                rowUV[x * 2 + 0] = srcU[x];
                rowUV[x * 2 + 1] = srcV[x];
            }
        }
    } else if (frame->fmt == VID_FMT_NV12) {
        for (unsigned y = 0; y < height; y++) {
            memcpy(dstY + y * dstYStride,
                   frame->data[0] + y * frame->linesize[0],
                   width);
        }
        for (unsigned y = 0; y < height / 2; y++) {
            memcpy(dstUV + y * dstUVStride,
                   frame->data[1] + y * frame->linesize[1],
                   width);
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    ves->current_rtp_ts = (timestamp * 90000ULL) / VIDEO_TIMEBASE;
    CMTime pts = CMTimeMake((int64_t)ves->frame_index++, 30);

    CFMutableDictionaryRef frameProps = nullptr;
    if (update || ves->frame_index == 1) {
        frameProps = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(frameProps, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
    }

    OSStatus status = VTCompressionSessionEncodeFrame(
        ves->session,
        pixelBuffer,
        pts,
        kCMTimeInvalid,
        frameProps,
        nullptr,
        nullptr);
    if (frameProps) {
        CFRelease(frameProps);
    }
    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr) {
        return EIO;
    }

    VTCompressionSessionCompleteFrames(ves->session, kCMTimeInvalid);

    std::vector<EncodedRtpPacket> ready;
    {
        std::lock_guard<std::mutex> lock(ves->pkt_mutex);
        ready.swap(ves->pending_packets);
    }
    if (ves->pkth) {
        for (const auto &pkt : ready) {
            ves->pkth(pkt.marker,
                      pkt.rtp_ts,
                      pkt.hdr.data(),
                      pkt.hdr.size(),
                      pkt.pld.data(),
                      pkt.pld.size(),
                      ves->vid);
        }
    }
    return 0;
}

static void vtDecompressionOutputCallback(void *decompressionOutputRefCon,
                                          void *sourceFrameRefCon,
                                          OSStatus status,
                                          VTDecodeInfoFlags infoFlags,
                                          CVImageBufferRef imageBuffer,
                                          CMTime presentationTimeStamp,
                                          CMTime presentationDuration)
{
    auto *vds = static_cast<struct viddec_state *>(decompressionOutputRefCon);
    if (!vds) {
        return;
    }
    if (status != noErr || !imageBuffer) {
        std::lock_guard<std::mutex> lock(vds->decode_mutex);
        vds->decode_failed = true;
        return;
    }

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    unsigned width = (unsigned)CVPixelBufferGetWidth(imageBuffer);
    unsigned height = (unsigned)CVPixelBufferGetHeight(imageBuffer);
    OSType pixelFormat = CVPixelBufferGetPixelFormatType(imageBuffer);

    if (width > 0 && height > 0 &&
        (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
         pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
        const uint8_t *srcY = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0);
        size_t srcYStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);
        const uint8_t *srcUV = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1);
        size_t srcUVStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

        struct vidsz sz = { width, height };
        size_t needed = vidframe_size(VID_FMT_YUV420P, &sz);

        std::lock_guard<std::mutex> lock(vds->decode_mutex);
        vds->decoded_yuv.resize(needed);
        vds->decoded_w = width;
        vds->decoded_h = height;

        uint8_t *dstY = vds->decoded_yuv.data();
        unsigned uvW = width / 2;
        unsigned uvH = height / 2;
        uint8_t *dstU = dstY + (size_t)width * (size_t)height;
        uint8_t *dstV = dstU + (size_t)uvW * (size_t)uvH;

        for (unsigned y = 0; y < height; y++) {
            memcpy(dstY + y * width, srcY + y * srcYStride, width);
        }
        for (unsigned y = 0; y < uvH; y++) {
            const uint8_t *rowUV = srcUV + y * srcUVStride;
            uint8_t *rowU = dstU + y * uvW;
            uint8_t *rowV = dstV + y * uvW;
            for (unsigned x = 0; x < uvW; x++) {
                rowU[x] = rowUV[x * 2 + 0];
                rowV[x] = rowUV[x * 2 + 1];
            }
        }
        vds->has_decoded_frame = true;

        SipClient *activeClient = gActiveSipClient;
        if (activeClient.remoteVideoView != nil) {
            [activeClient.remoteVideoView renderYUV420WithY:dstY
                                                          u:dstU
                                                          v:dstV
                                                    yStride:(int)width
                                                    uStride:(int)uvW
                                                    vStride:(int)uvW
                                                      width:(int)width
                                                     height:(int)height];
        }
    }
    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

static void h264_dec_destructor(void *arg)
{
    auto *vds = static_cast<struct viddec_state *>(arg);
    if (vds->session) {
        VTDecompressionSessionInvalidate(vds->session);
        CFRelease(vds->session);
        vds->session = nullptr;
    }
    if (vds->formatDesc) {
        CFRelease(vds->formatDesc);
        vds->formatDesc = nullptr;
    }
    vds->~viddec_state();
}

static int h264_dec_update(struct viddec_state **vdsp,
                           const struct vidcodec *vc,
                           const char *fmtp,
                           const struct video *vid)
{
    (void)vc;
    (void)fmtp;
    (void)vid;
    if (!vdsp) {
        return EINVAL;
    }

    struct viddec_state *vds = *vdsp;
    if (!vds) {
        void *raw = mem_zalloc(sizeof(struct viddec_state), h264_dec_destructor);
        if (!raw) {
            return ENOMEM;
        }
        vds = new (raw) struct viddec_state();
        vds->session = nullptr;
        vds->formatDesc = nullptr;
        vds->fu_active = false;
        vds->au_is_idr = false;
        vds->decoded_w = 0;
        vds->decoded_h = 0;
        vds->has_decoded_frame = false;
        vds->have_seq = false;
        vds->last_seq = 0;
        vds->have_au_ts = false;
        vds->au_ts = 0;
        vds->au_corrupt = false;
        vds->need_idr = true; // nothing decodes before the first IDR
        vds->decode_failed = false;
        vds->last_picup_ms = 0;
        *vdsp = vds;
    }
    return 0;
}

static void appendNalToAccessUnit(struct viddec_state *vds, const uint8_t *nal, size_t len)
{
    if (!vds || !nal || len == 0) {
        return;
    }
    uint8_t nalType = nal[0] & 0x1F;
    if (nalType == H264_NALU_SPS) {
        vds->sps.assign(nal, nal + len);
        return;
    }
    if (nalType == H264_NALU_PPS) {
        vds->pps.assign(nal, nal + len);
        return;
    }
    if (nalType == H264_NALU_IDR_SLICE) {
        vds->au_is_idr = true;
    }
    if (nalType >= H264_NALU_SLICE && nalType <= H264_NALU_IDR_SLICE) {
        uint32_t beLen = CFSwapInt32HostToBig((uint32_t)len);
        const uint8_t *lenBytes = reinterpret_cast<const uint8_t *>(&beLen);
        vds->au_buffer.insert(vds->au_buffer.end(), lenBytes, lenBytes + 4);
        vds->au_buffer.insert(vds->au_buffer.end(), nal, nal + len);
    }
}

static int ensureDecoderSession(struct viddec_state *vds)
{
    if (vds->sps.empty() || vds->pps.empty()) {
        return EINVAL;
    }

    if (vds->session && vds->formatDesc &&
        vds->sps == vds->format_sps && vds->pps == vds->format_pps) {
        return 0;
    }

    if (vds->session) {
        VTDecompressionSessionInvalidate(vds->session);
        CFRelease(vds->session);
        vds->session = nullptr;
    }
    if (vds->formatDesc) {
        CFRelease(vds->formatDesc);
        vds->formatDesc = nullptr;
    }

    const uint8_t *paramPtrs[2] = { vds->sps.data(), vds->pps.data() };
    size_t paramSizes[2] = { vds->sps.size(), vds->pps.size() };

    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault,
        2,
        paramPtrs,
        paramSizes,
        4,
        &vds->formatDesc);
    if (status != noErr || !vds->formatDesc) {
        return EIO;
    }

    OSType pixFmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pixFmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixFmt);
    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey };
    const void *values[] = { pixFmtNum };
    CFDictionaryRef destAttrs = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFRelease(pixFmtNum);

    VTDecompressionOutputCallbackRecord callRecord = {
        vtDecompressionOutputCallback,
        vds
    };

    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        vds->formatDesc,
        nullptr,
        destAttrs,
        &callRecord,
        &vds->session);
    CFRelease(destAttrs);

    if (status != noErr || !vds->session) {
        return EIO;
    }

    vds->format_sps = vds->sps;
    vds->format_pps = vds->pps;
    return 0;
}

static int h264_dec_decode(struct viddec_state *vds,
                           struct vidframe *frame,
                           struct viddec_packet *pkt)
{
    if (!vds || !frame || !pkt || !pkt->mb) {
        return EINVAL;
    }

    const uint8_t *buf = mbuf_buf(pkt->mb);
    size_t len = mbuf_get_left(pkt->mb);
    if (!buf || len == 0) {
        return 0;
    }

    if (pkt->hdr) {
        // A new RTP timestamp while an access unit is still open means the
        // marker packet of the previous frame was lost.
        if (vds->have_au_ts && pkt->hdr->ts != vds->au_ts &&
            (!vds->au_buffer.empty() || vds->fu_active)) {
            vds->au_buffer.clear();
            vds->au_is_idr = false;
            vds->fu_buffer.clear();
            vds->fu_active = false;
            vds->need_idr = true;
        }
        vds->au_ts = pkt->hdr->ts;
        vds->have_au_ts = true;

        if (vds->have_seq && pkt->hdr->seq != (uint16_t)(vds->last_seq + 1)) {
            vds->au_corrupt = true;
            vds->fu_buffer.clear();
            vds->fu_active = false;
        }
        vds->last_seq = pkt->hdr->seq;
        vds->have_seq = true;
    }

    uint8_t nalType = buf[0] & 0x1F;
    if (nalType >= 1 && nalType <= 23) {
        appendNalToAccessUnit(vds, buf, len);
    } else if (nalType == H264_NALU_STAP_A) {
        size_t offset = 1;
        while (offset + 2 <= len) {
            uint16_t naluSize = ((uint16_t)buf[offset] << 8) | (uint16_t)buf[offset + 1];
            offset += 2;
            if (naluSize == 0 || offset + naluSize > len) {
                break;
            }
            appendNalToAccessUnit(vds, buf + offset, naluSize);
            offset += naluSize;
        }
    } else if (nalType == H264_NALU_FU_A && len >= 2) {
        uint8_t fuIndicator = buf[0];
        uint8_t fuHeader = buf[1];
        uint8_t origType = fuHeader & 0x1F;
        bool startBit = (fuHeader & 0x80) != 0;
        bool endBit = (fuHeader & 0x40) != 0;

        if (startBit) {
            vds->fu_buffer.clear();
            uint8_t reconstructedNalHdr = (fuIndicator & 0xE0) | origType;
            vds->fu_buffer.push_back(reconstructedNalHdr);
            vds->fu_buffer.insert(vds->fu_buffer.end(), buf + 2, buf + len);
            vds->fu_active = true;
        } else if (vds->fu_active) {
            vds->fu_buffer.insert(vds->fu_buffer.end(), buf + 2, buf + len);
        } else {
            vds->au_corrupt = true; // middle/end fragment without its start
        }

        if (endBit && vds->fu_active) {
            appendNalToAccessUnit(vds, vds->fu_buffer.data(), vds->fu_buffer.size());
            vds->fu_buffer.clear();
            vds->fu_active = false;
        }
    }

    bool marker = (pkt->hdr != nullptr) ? pkt->hdr->m : false;
    if (!marker) {
        return 0;
    }

    bool corrupt = vds->au_corrupt;
    vds->au_corrupt = false;
    if (corrupt) {
        vds->need_idr = true;
    }

    if (vds->au_buffer.empty() && !vds->need_idr) {
        return 0;
    }

    pkt->intra = vds->au_is_idr;

    if (vds->au_is_idr && !corrupt) {
        vds->need_idr = false;
    }
    if (vds->need_idr) {
        // Drop frames that reference missing data. Return an error so Baresip
        // sends RTCP PLI/FIR; limit it to one request per 500 ms.
        vds->au_buffer.clear();
        vds->au_is_idr = false;
        uint64_t now = tmr_jiffies();
        if (vds->last_picup_ms == 0 || now - vds->last_picup_ms >= 500) {
            vds->last_picup_ms = now;
            return EPROTO;
        }
        return 0;
    }

    int result = 0;
    if (ensureDecoderSession(vds) == 0) {
        CMBlockBufferRef blockBuffer = nullptr;
        size_t auSize = vds->au_buffer.size();
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault,
            nullptr,
            auSize,
            kCFAllocatorDefault,
            nullptr,
            0,
            auSize,
            kCMBlockBufferAssureMemoryNowFlag,
            &blockBuffer);
        if (status == noErr && blockBuffer) {
            CMBlockBufferReplaceDataBytes(vds->au_buffer.data(), blockBuffer, 0, auSize);
            CMSampleBufferRef sampleBuffer = nullptr;
            const size_t sampleSize = auSize;
            status = CMSampleBufferCreateReady(
                kCFAllocatorDefault,
                blockBuffer,
                vds->formatDesc,
                1,
                0,
                nullptr,
                1,
                &sampleSize,
                &sampleBuffer);
            if (status == noErr && sampleBuffer) {
                {
                    std::lock_guard<std::mutex> lock(vds->decode_mutex);
                    vds->has_decoded_frame = false;
                    vds->decode_failed = false;
                }
                VTDecodeInfoFlags flagOut = 0;
                OSStatus decStatus = VTDecompressionSessionDecodeFrame(
                    vds->session,
                    sampleBuffer,
                    0,
                    nullptr,
                    &flagOut);
                VTDecompressionSessionWaitForAsynchronousFrames(vds->session);
                CFRelease(sampleBuffer);

                std::lock_guard<std::mutex> lock(vds->decode_mutex);
                if (decStatus != noErr || vds->decode_failed) {
                    vds->need_idr = true;
                    vds->last_picup_ms = tmr_jiffies();
                    result = EPROTO;
                } else if (vds->has_decoded_frame && vds->decoded_w > 0 && vds->decoded_h > 0) {
                    struct vidsz sz = { vds->decoded_w, vds->decoded_h };
                    vidframe_init_buf(frame, VID_FMT_YUV420P, &sz, vds->decoded_yuv.data());
                }
            }
            CFRelease(blockBuffer);
        }
    }

    vds->au_buffer.clear();
    vds->au_is_idr = false;
    return result;
}

static struct vidcodec gH264CodecMode1 = {
    .le = LE_INIT,
    .pt = "96",
    .name = "H264",
    .variant = "packetization-mode=1",
    .fmtp = "packetization-mode=1;profile-level-id=42e01f",
    .encupdh = h264_enc_update,
    .ench = h264_enc_encode,
    .decupdh = h264_dec_update,
    .dech = h264_dec_decode,
    .fmtp_ench = nullptr,
    .fmtp_cmph = nullptr,
    .packetizeh = nullptr,
};

static struct vidcodec gH264CodecMode0 = {
    .le = LE_INIT,
    .pt = "97",
    .name = "H264",
    .variant = "packetization-mode=0",
    .fmtp = "packetization-mode=0;profile-level-id=42e01f",
    .encupdh = h264_enc_update,
    .ench = h264_enc_encode,
    .decupdh = h264_dec_update,
    .dech = h264_dec_decode,
    .fmtp_ench = nullptr,
    .fmtp_cmph = nullptr,
    .packetizeh = nullptr,
};

// ============================================================================
// MARK: - IosVideoCodecDriver Implementation
// ============================================================================

static int ios_codec_register(void *user_data)
{
    if (!user_data) return EINVAL;
    IosVideoCodecDriver *d = (__bridge IosVideoCodecDriver *)user_data;
    return [d registerCodec];
}

static void ios_codec_unregister(void *user_data)
{
    if (!user_data) return;
    IosVideoCodecDriver *d = (__bridge IosVideoCodecDriver *)user_data;
    [d unregisterCodec];
}

@implementation IosVideoCodecDriver

- (sip_video_codec_driver_t)cDriver
{
    sip_video_codec_driver_t driver{};
    driver.user_data = (__bridge void *)self;
    driver.register_codec = ios_codec_register;
    driver.unregister_codec = ios_codec_unregister;
    return driver;
}

- (int)registerCodec
{
    vidcodec_register(baresip_vidcodecl(), &gH264CodecMode1);
    vidcodec_register(baresip_vidcodecl(), &gH264CodecMode0);
    return 0;
}

- (void)unregisterCodec
{
    vidcodec_unregister(&gH264CodecMode0);
    vidcodec_unregister(&gH264CodecMode1);
}

@end

// ============================================================================
// MARK: - Native iOS Video Capture (vidsrc "avcapture")
// ============================================================================

@interface SipAVCaptureHelper : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) struct vidsrc_st *owner;
@property (nonatomic, assign) unsigned width;
@property (nonatomic, assign) unsigned height;
@property (nonatomic, assign) double fps;
@property (nonatomic, strong, nullable) AVCaptureSession *captureSession;
@property (nonatomic, strong, nullable) dispatch_source_t syntheticTimer;
@property (nonatomic, strong) dispatch_queue_t captureQueue;
@property (nonatomic, assign) uint32_t syntheticTick;
- (void)startWithDeviceName:(NSString *)devName;
- (void)switchToDeviceName:(NSString *)devName;
- (void)stop;
@end

struct vidsrc_st {
    const struct vidsrc *vs;
    vidsrc_frame_h *frameh;
    void *arg;
    struct vidsz size;
    double fps;
    void *helperRef;
};

static struct vidsrc *gIosVidSrc = nullptr;
static __weak SipAVCaptureHelper *gActiveCaptureHelper = nil;

@implementation SipAVCaptureHelper

- (instancetype)initWithOwner:(struct vidsrc_st *)owner
                        width:(unsigned)width
                       height:(unsigned)height
                          fps:(double)fps
{
    self = [super init];
    if (self) {
        _owner = owner;
        _width = width > 0 ? width : 640;
        _height = height > 0 ? height : 480;
        _fps = fps > 0 ? fps : 30.0;
        _captureQueue = dispatch_queue_create("com.wiseland.SipSdk.avcapture", DISPATCH_QUEUE_SERIAL);
        _syntheticTick = 0;
        gActiveCaptureHelper = self;
    }
    return self;
}

- (nullable AVCaptureDevice *)cameraForPosition:(AVCaptureDevicePosition)position
{
    AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                              mediaType:AVMediaTypeVideo
                               position:position];
    return discovery.devices.firstObject;
}

- (void)configureVideoConnection:(AVCaptureConnection *)connection
                     forPosition:(AVCaptureDevicePosition)position
{
    if (connection == nil) {
        return;
    }
    BOOL rotated = NO;
    if (@available(iOS 17.0, *)) {
        const CGFloat portraitAngle = 90.0;
        if ([connection isVideoRotationAngleSupported:portraitAngle]) {
            connection.videoRotationAngle = portraitAngle;
            rotated = YES;
        }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!rotated && connection.isVideoOrientationSupported) {
        connection.videoOrientation = AVCaptureVideoOrientationPortrait;
    }
#pragma clang diagnostic pop
    if (position == AVCaptureDevicePositionFront && connection.isVideoMirroringSupported) {
        connection.automaticallyAdjustsVideoMirroring = NO;
        connection.videoMirrored = YES;
    }
}

- (void)startSyntheticFallbackTimer
{
    if (self.syntheticTimer) {
        return;
    }
    dispatch_source_t timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.captureQueue);
    uint64_t intervalNs = (uint64_t)((1.0 / (self.fps > 0 ? self.fps : 30.0)) * (double)NSEC_PER_SEC);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), intervalNs, intervalNs / 10);
    __weak SipAVCaptureHelper *weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        [weakSelf emitSyntheticFrame];
    });
    self.syntheticTimer = timer;
    dispatch_resume(timer);
}

- (void)startWithDeviceName:(NSString *)devName
{
    NSString *requestedDev = [devName copy] ?: @"front";
    dispatch_async(self.captureQueue, ^{
        [self stopInternal];

        // Start synthetic 30 fps frame timer immediately so local/remote video pipelines
        // never stall while hardware camera session starts or awaits authorization.
        [self startSyntheticFallbackTimer];

        AVAuthorizationStatus auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (auth == AVAuthorizationStatusNotDetermined) {
            __weak SipAVCaptureHelper *weakSelf = self;
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
                if (granted) {
                    [weakSelf startWithDeviceName:requestedDev];
                }
            }];
            return;
        }

        if (auth != AVAuthorizationStatusAuthorized) {
            return;
        }

        AVCaptureDevicePosition wantedPos = ([requestedDev.lowercaseString containsString:@"back"])
            ? AVCaptureDevicePositionBack
            : AVCaptureDevicePositionFront;

        AVCaptureDevice *device = [self cameraForPosition:wantedPos];
        if (!device) {
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }

        if (device != nil) {
            NSError *inputErr = nil;
            AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&inputErr];
            if (input != nil && inputErr == nil) {
                AVCaptureSession *session = [[AVCaptureSession alloc] init];
                [session beginConfiguration];
                if ([session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
                    session.sessionPreset = AVCaptureSessionPreset640x480;
                }
                if ([session canAddInput:input]) {
                    [session addInput:input];
                }
                AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
                output.alwaysDiscardsLateVideoFrames = YES;
                output.videoSettings = @{
                    (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                };
                [output setSampleBufferDelegate:self queue:self.captureQueue];
                if ([session canAddOutput:output]) {
                    [session addOutput:output];
                }
                [session commitConfiguration];
                [self configureVideoConnection:[output connectionWithMediaType:AVMediaTypeVideo]
                                   forPosition:wantedPos];
                [session startRunning];
                self.captureSession = session;
                return;
            }
        }
    });
}

- (void)switchToDeviceName:(NSString *)devName
{
    [self startWithDeviceName:devName];
}

- (void)emitSyntheticFrame
{
    struct vidsrc_st *st = self.owner;
    SipClient *client = gActiveSipClient;
    if ((!st || !st->frameh) && client.localVideoView == nil) {
        return;
    }

    unsigned w = self.width;
    unsigned h = self.height;
    unsigned uvW = w / 2;
    unsigned uvH = h / 2;

    struct vidsz sz = { w, h };
    size_t bufSize = vidframe_size(VID_FMT_YUV420P, &sz);
    std::vector<uint8_t> buf(bufSize, 0);

    uint8_t *yPlane = buf.data();
    uint8_t *uPlane = yPlane + (size_t)w * (size_t)h;
    uint8_t *vPlane = uPlane + (size_t)uvW * (size_t)uvH;

    if (client == nil || client.isLocalVideoEnabled) {
        fillSyntheticYUV420Pattern(yPlane, uPlane, vPlane, (int)w, (int)h, ++self.syntheticTick);
    } else {
        memset(yPlane, 16, (size_t)w * (size_t)h);
        memset(uPlane, 128, (size_t)uvW * (size_t)uvH);
        memset(vPlane, 128, (size_t)uvW * (size_t)uvH);
    }

    if (client.localVideoView != nil && client.isLocalVideoEnabled) {
        [client.localVideoView renderYUV420WithY:yPlane
                                               u:uPlane
                                               v:vPlane
                                         yStride:(int)w
                                         uStride:(int)uvW
                                         vStride:(int)uvW
                                           width:(int)w
                                          height:(int)h];
    }

    if (client.captureDriver != nil) {
        sip_video_frame_t cvf{};
        cvf.width = (int)w;
        cvf.height = (int)h;
        cvf.y = yPlane;
        cvf.u = uPlane;
        cvf.v = vPlane;
        cvf.y_stride = (int)w;
        cvf.u_stride = (int)uvW;
        cvf.v_stride = (int)uvW;
        cvf.timestamp_ns = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * 1e9);
        [client.captureDriver deliverCustomFrame:&cvf];
    }

    if (!st || !st->frameh) {
        return;
    }
    struct vidframe vf;
    vidframe_init_buf(&vf, VID_FMT_YUV420P, &sz, buf.data());
    uint64_t ts = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * (double)VIDEO_TIMEBASE);
    std::vector<uint8_t> frameCopy = std::move(buf);
    SipAVCaptureHelper *strongSelf = self;
    performAsyncOnSipLoop(client, ^{
        struct vidsrc_st *loopSt = strongSelf.owner;
        if (!loopSt || !loopSt->frameh) {
            return;
        }
        struct vidsz loopSz = { w, h };
        struct vidframe loopVf;
        vidframe_init_buf(&loopVf, VID_FMT_YUV420P, &loopSz, (uint8_t *)frameCopy.data());
        loopSt->frameh(&loopVf, ts, loopSt->arg);
    });
}

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;
    if (self.syntheticTimer) {
        dispatch_source_cancel(self.syntheticTimer);
        self.syntheticTimer = nil;
    }
    struct vidsrc_st *st = self.owner;
    SipClient *client = gActiveSipClient;
    if ((!st || !st->frameh) && client.localVideoView == nil) {
        return;
    }
    if (client != nil && !client.isLocalVideoEnabled) {
        return;
    }

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) {
        return;
    }

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    unsigned w = (unsigned)CVPixelBufferGetWidth(imageBuffer);
    unsigned h = (unsigned)CVPixelBufferGetHeight(imageBuffer);
    const uint8_t *srcY = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0);
    size_t srcYStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);
    const uint8_t *srcUV = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1);
    size_t srcUVStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

    if (w > 0 && h > 0 && srcY && srcUV) {
        struct vidsz sz = { w, h };
        size_t bufSize = vidframe_size(VID_FMT_YUV420P, &sz);
        std::vector<uint8_t> buf(bufSize);

        uint8_t *dstY = buf.data();
        unsigned uvW = w / 2;
        unsigned uvH = h / 2;
        uint8_t *dstU = dstY + (size_t)w * (size_t)h;
        uint8_t *dstV = dstU + (size_t)uvW * (size_t)uvH;

        for (unsigned y = 0; y < h; y++) {
            memcpy(dstY + y * w, srcY + y * srcYStride, w);
        }
        for (unsigned y = 0; y < uvH; y++) {
            const uint8_t *rowUV = srcUV + y * srcUVStride;
            uint8_t *rowU = dstU + y * uvW;
            uint8_t *rowV = dstV + y * uvW;
            for (unsigned x = 0; x < uvW; x++) {
                rowU[x] = rowUV[x * 2 + 0];
                rowV[x] = rowUV[x * 2 + 1];
            }
        }

        if (client.localVideoView != nil) {
            [client.localVideoView renderYUV420WithY:dstY
                                                   u:dstU
                                                   v:dstV
                                             yStride:(int)w
                                             uStride:(int)uvW
                                             vStride:(int)uvW
                                               width:(int)w
                                              height:(int)h];
        }

        if (client.captureDriver != nil) {
            [client.captureDriver deliverSampleBuffer:sampleBuffer];
        }

        if (st && st->frameh) {
            uint64_t ts = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * (double)VIDEO_TIMEBASE);
            std::vector<uint8_t> frameCopy = std::move(buf);
            SipAVCaptureHelper *strongSelf = self;
            performAsyncOnSipLoop(client, ^{
                struct vidsrc_st *loopSt = strongSelf.owner;
                if (!loopSt || !loopSt->frameh) {
                    return;
                }
                struct vidsz loopSz = { w, h };
                struct vidframe loopVf;
                vidframe_init_buf(&loopVf, VID_FMT_YUV420P, &loopSz, (uint8_t *)frameCopy.data());
                loopSt->frameh(&loopVf, ts, loopSt->arg);
            });
        }
    }
    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

- (void)stopInternal
{
    if (self.syntheticTimer) {
        dispatch_source_cancel(self.syntheticTimer);
        self.syntheticTimer = nil;
    }
    if (self.captureSession) {
        if (self.captureSession.isRunning) {
            [self.captureSession stopRunning];
        }
        self.captureSession = nil;
    }
}

- (void)stop
{
    self.owner = nullptr;
    if (gActiveCaptureHelper == self) {
        gActiveCaptureHelper = nil;
    }
    dispatch_async(self.captureQueue, ^{
        [self stopInternal];
    });
}

@end

// ============================================================================
// MARK: - IosVideoCaptureDriver Implementation
// ============================================================================

typedef void (*sip_frame_handler_fn)(const sip_video_frame_t *frame, void *arg);

@interface IosVideoCaptureDriver ()
@property (nonatomic, assign) BOOL enabled;
@property (nonatomic, assign) int cameraPosition;
@property (nonatomic, assign) int width;
@property (nonatomic, assign) int height;
@property (nonatomic, assign) int fps;
@property (nonatomic, assign) sip_frame_handler_fn frameHandler;
@property (nonatomic, assign) void *frameHandlerArg;
@property (nonatomic, strong) dispatch_queue_t captureQueue;
@property (nonatomic, strong, nullable) dispatch_source_t syntheticTimer;
@property (nonatomic, assign) uint32_t syntheticTick;
- (void)deliverCustomFrame:(const sip_video_frame_t *)frame;
- (void)deliverSampleBuffer:(CMSampleBufferRef)sampleBuffer;
- (void)setFrameHandler:(sip_frame_handler_fn)handler argument:(void *)arg;
@end

static int ios_capture_start(void *user_data, int width, int height, int fps)
{
    if (!user_data) return EINVAL;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    return [d startWithWidth:width height:height fps:fps];
}

static void ios_capture_stop(void *user_data)
{
    if (!user_data) return;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    [d stop];
}

static void ios_capture_set_frame_handler(void *user_data, void (*handler)(const sip_video_frame_t *frame, void *arg), void *arg)
{
    if (!user_data) return;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    [d setFrameHandler:handler argument:arg];
}

static void ios_capture_switch_camera(void *user_data)
{
    if (!user_data) return;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    [d switchCamera];
}

static void ios_capture_set_camera(void *user_data, int position)
{
    if (!user_data) return;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    [d setCamera:position];
}

static void ios_capture_set_enabled(void *user_data, bool enabled)
{
    if (!user_data) return;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    [d setEnabled:enabled ? YES : NO];
}

static bool ios_capture_is_enabled(void *user_data)
{
    if (!user_data) return false;
    IosVideoCaptureDriver *d = (__bridge IosVideoCaptureDriver *)user_data;
    return [d isEnabled];
}

@implementation IosVideoCaptureDriver

- (instancetype)initWithClient:(nullable SipClient *)client
{
    self = [super init];
    if (self) {
        _client = client;
        _enabled = YES;
        _cameraPosition = 0; // Front
        _width = 640;
        _height = 480;
        _fps = 30;
        _syntheticTick = 0;
        _captureQueue = dispatch_queue_create("com.wiseland.SipSdk.captureDriver", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (sip_video_capture_driver_t)cDriver
{
    sip_video_capture_driver_t driver{};
    driver.user_data = (__bridge void *)self;
    driver.start = ios_capture_start;
    driver.stop = ios_capture_stop;
    driver.set_frame_handler = ios_capture_set_frame_handler;
    driver.switch_camera = ios_capture_switch_camera;
    driver.set_camera = ios_capture_set_camera;
    driver.set_enabled = ios_capture_set_enabled;
    driver.is_enabled = ios_capture_is_enabled;
    return driver;
}

- (void)setFrameHandler:(sip_frame_handler_fn)handler argument:(void *)arg
{
    _frameHandler = handler;
    _frameHandlerArg = arg;
}

- (BOOL)isEnabled
{
    return _enabled;
}

- (int)startWithWidth:(int)width height:(int)height fps:(int)fps
{
    _width = width > 0 ? width : 640;
    _height = height > 0 ? height : 480;
    _fps = fps > 0 ? fps : 30;

    dispatch_async(self.captureQueue, ^{
        [self stopSyntheticTimerInternal];

        dispatch_source_t timer = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.captureQueue);
        uint64_t intervalNs = (uint64_t)((1.0 / (double)self.fps) * (double)NSEC_PER_SEC);
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), intervalNs, intervalNs / 10);
        __weak IosVideoCaptureDriver *weakSelf = self;
        dispatch_source_set_event_handler(timer, ^{
            [weakSelf deliverSyntheticFrame];
        });
        self.syntheticTimer = timer;
        dispatch_resume(timer);
    });

    return 0;
}

- (void)stopSyntheticTimerInternal
{
    if (self.syntheticTimer) {
        dispatch_source_cancel(self.syntheticTimer);
        self.syntheticTimer = nil;
    }
}

- (void)stop
{
    dispatch_sync(self.captureQueue, ^{
        [self stopSyntheticTimerInternal];
    });
}

- (void)switchCamera
{
    _cameraPosition = (_cameraPosition == 0) ? 1 : 0;
}

- (void)setCamera:(int)position
{
    _cameraPosition = position;
}

- (void)deliverSyntheticFrame
{
    int w = self.width > 0 ? self.width : 640;
    int h = self.height > 0 ? self.height : 480;
    int uvW = w / 2;
    int uvH = h / 2;

    size_t ySize = (size_t)w * (size_t)h;
    size_t uvSize = (size_t)uvW * (size_t)uvH;
    std::vector<uint8_t> buf(ySize + uvSize * 2, 0);

    uint8_t *yPlane = buf.data();
    uint8_t *uPlane = yPlane + ySize;
    uint8_t *vPlane = uPlane + uvSize;

    if (self.enabled) {
        fillSyntheticYUV420Pattern(yPlane, uPlane, vPlane, w, h, ++self.syntheticTick);
    } else {
        memset(yPlane, 16, ySize);
        memset(uPlane, 128, uvSize);
        memset(vPlane, 128, uvSize);
    }

    SipClient *client = self.client;
    if (client.localVideoView != nil && self.enabled) {
        [client.localVideoView renderYUV420WithY:yPlane
                                               u:uPlane
                                               v:vPlane
                                         yStride:w
                                         uStride:uvW
                                         vStride:uvW
                                           width:w
                                          height:h];
    }

    if (self.frameHandler != nullptr) {
        sip_video_frame_t vf{};
        vf.width = w;
        vf.height = h;
        vf.y = yPlane;
        vf.u = uPlane;
        vf.v = vPlane;
        vf.y_stride = w;
        vf.u_stride = uvW;
        vf.v_stride = uvW;
        vf.timestamp_ns = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * 1e9);
        self.frameHandler(&vf, self.frameHandlerArg);
    }
}

- (void)deliverCustomFrame:(const sip_video_frame_t *)frame
{
    if (!frame) return;
    if (_frameHandler != nullptr && _enabled) {
        _frameHandler(frame, _frameHandlerArg);
    }
}

- (void)deliverSampleBuffer:(CMSampleBufferRef)sampleBuffer
{
    if (!sampleBuffer) return;
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    int width = (int)CVPixelBufferGetWidth(imageBuffer);
    int height = (int)CVPixelBufferGetHeight(imageBuffer);
    int uvWidth = width / 2;
    int uvHeight = height / 2;

    const uint8_t *yPlane = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0);
    int yStride = (int)CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);
    const uint8_t *uvPlane = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1);
    int uvStride = (int)CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

    if (yPlane && uvPlane) {
        std::vector<uint8_t> uPlaneVec((size_t)uvWidth * (size_t)uvHeight);
        std::vector<uint8_t> vPlaneVec((size_t)uvWidth * (size_t)uvHeight);
        for (int row = 0; row < uvHeight; row++) {
            const uint8_t *srcRow = uvPlane + row * uvStride;
            uint8_t *uDst = uPlaneVec.data() + row * uvWidth;
            uint8_t *vDst = vPlaneVec.data() + row * uvWidth;
            for (int col = 0; col < uvWidth; col++) {
                uDst[col] = srcRow[col * 2 + 0];
                vDst[col] = srcRow[col * 2 + 1];
            }
        }

        SipClient *client = self.client;
        if (client.localVideoView != nil && self.enabled) {
            [client.localVideoView renderYUV420WithY:yPlane
                                                   u:uPlaneVec.data()
                                                   v:vPlaneVec.data()
                                             yStride:yStride
                                             uStride:uvWidth
                                             vStride:uvWidth
                                               width:width
                                              height:height];
        }

        if (self.frameHandler != nullptr && self.enabled) {
            sip_video_frame_t vf{};
            vf.width = width;
            vf.height = height;
            vf.y = yPlane;
            vf.u = uPlaneVec.data();
            vf.v = vPlaneVec.data();
            vf.y_stride = yStride;
            vf.u_stride = uvWidth;
            vf.v_stride = uvWidth;
            vf.timestamp_ns = (uint64_t)([[NSProcessInfo processInfo] systemUptime] * 1e9);
            self.frameHandler(&vf, self.frameHandlerArg);
        }
    }
    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

static void ios_vidsrc_destructor(void *arg)
{
    auto *st = static_cast<struct vidsrc_st *>(arg);
    if (st->helperRef) {
        SipAVCaptureHelper *helper = (__bridge_transfer SipAVCaptureHelper *)st->helperRef;
        st->helperRef = nullptr;
        [helper stop];
    }
}

static int ios_vidsrc_alloc(struct vidsrc_st **vsp,
                            const struct vidsrc *vs,
                            struct vidsrc_prm *prm,
                            const struct vidsz *size,
                            const char *fmt,
                            const char *dev,
                            vidsrc_frame_h *frameh,
                            vidsrc_packet_h *packeth,
                            vidsrc_error_h *errorh,
                            void *arg)
{
    (void)fmt;
    (void)packeth;
    (void)errorh;
    if (!vsp || !size || !frameh) {
        return EINVAL;
    }

    struct vidsrc_st *st = (struct vidsrc_st *)mem_zalloc(sizeof(*st), ios_vidsrc_destructor);
    if (!st) {
        return ENOMEM;
    }

    st->vs = vs;
    st->frameh = frameh;
    st->arg = arg;
    st->size = *size;
    st->fps = (prm && prm->fps > 0) ? prm->fps : 30.0;

    SipAVCaptureHelper *helper = [[SipAVCaptureHelper alloc]
        initWithOwner:st
                width:size->w
               height:size->h
                  fps:st->fps];
    st->helperRef = (__bridge_retained void *)helper;

    NSString *deviceName = dev ? [NSString stringWithUTF8String:dev] : @"front";
    [helper startWithDeviceName:deviceName];

    *vsp = st;
    return 0;
}

static void ios_vidsrc_update(struct vidsrc_st *st,
                              struct vidsrc_prm *prm,
                              const char *dev)
{
    (void)prm;
    if (!st || !st->helperRef) {
        return;
    }
    SipAVCaptureHelper *helper = (__bridge SipAVCaptureHelper *)st->helperRef;
    NSString *deviceName = dev ? [NSString stringWithUTF8String:dev] : @"front";
    [helper switchToDeviceName:deviceName];
}

// ============================================================================
// MARK: - Native iOS Video Display (vidisp "iosview")
// ============================================================================

struct vidisp_st {
    const struct vidisp *vd;
    vidisp_resize_h *resizeh;
    void *arg;
    struct vidsz last_size;
};

static struct vidisp *gIosVidIsp = nullptr;

static void ios_vidisp_destructor(void *arg)
{
    (void)arg;
}

static int ios_vidisp_alloc(struct vidisp_st **vp,
                            const struct vidisp *vd,
                            struct vidisp_prm *prm,
                            const char *dev,
                            vidisp_resize_h *resizeh,
                            void *arg)
{
    (void)prm;
    (void)dev;
    if (!vp) {
        return EINVAL;
    }

    struct vidisp_st *st = (struct vidisp_st *)mem_zalloc(sizeof(*st), ios_vidisp_destructor);
    if (!st) {
        return ENOMEM;
    }
    st->vd = vd;
    st->resizeh = resizeh;
    st->arg = arg;
    st->last_size.w = 0;
    st->last_size.h = 0;
    *vp = st;
    return 0;
}

static int ios_vidisp_update(struct vidisp_st *st,
                             bool fullscreen,
                             int orient,
                             const struct vidrect *window)
{
    (void)st;
    (void)fullscreen;
    (void)orient;
    (void)window;
    return 0;
}

static int ios_vidisp_display(struct vidisp_st *st,
                              const char *title,
                              const struct vidframe *frame,
                              uint64_t timestamp)
{
    (void)title;
    (void)timestamp;
    if (!st || !frame || !vidframe_isvalid(frame)) {
        return EINVAL;
    }

    unsigned w = frame->size.w;
    unsigned h = frame->size.h;
    if (w == 0 || h == 0) {
        return EINVAL;
    }

    SipClient *client = gActiveSipClient;
    if (st->last_size.w != w || st->last_size.h != h) {
        st->last_size = frame->size;
        if (st->resizeh) {
            st->resizeh(&frame->size, st->arg);
        }
        if (client.remoteRendererDriver != nil) {
            [client.remoteRendererDriver onStreamResizedWithWidth:(int)w height:(int)h];
        }
    }

    if (frame->fmt == VID_FMT_YUV420P) {
        if (client.remoteRendererDriver != nil) {
            sip_video_frame_t vf{};
            vf.width = (int)w;
            vf.height = (int)h;
            vf.y = frame->data[0];
            vf.u = frame->data[1];
            vf.v = frame->data[2];
            vf.y_stride = (int)frame->linesize[0];
            vf.u_stride = (int)frame->linesize[1];
            vf.v_stride = (int)frame->linesize[2];
            vf.timestamp_ns = timestamp > 0 ? (timestamp * 1000ULL) : 0;
            [client.remoteRendererDriver renderFrame:&vf];
        } else if (client.remoteVideoView != nil) {
            [client.remoteVideoView renderYUV420WithY:frame->data[0]
                                                    u:frame->data[1]
                                                    v:frame->data[2]
                                              yStride:frame->linesize[0]
                                              uStride:frame->linesize[1]
                                              vStride:frame->linesize[2]
                                                width:(int)w
                                               height:(int)h];
        }
    }
    return 0;
}

static void ios_vidisp_hide(struct vidisp_st *st)
{
    (void)st;
    SipClient *client = gActiveSipClient;
    if (client.remoteRendererDriver != nil) {
        [client.remoteRendererDriver clear];
    } else if (client.remoteVideoView != nil) {
        [client.remoteVideoView clearVideo];
    }
}

// ============================================================================
// MARK: - SipCall Cache & Baresip Event Handler
// ============================================================================

static NSMutableDictionary<NSValue *, SipCall *> *sipCallsCache(void)
{
    static NSMutableDictionary<NSValue *, SipCall *> *cache;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        cache = [[NSMutableDictionary alloc] init];
    });
    return cache;
}

static NSValue *sipCallCacheKey(struct call *call)
{
    return [NSValue valueWithPointer:call];
}

static SipCall *getSipCall(struct call *call, SipClient *sipSdk, NSString *remoteUri)
{
    if (call == nullptr) {
        return nil;
    }

    NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
    NSValue *key = sipCallCacheKey(call);

    @synchronized (cache) {
        SipCall *sipCall = cache[key];
        if (sipCall == nil) {
            sipCall = [[SipCall alloc] init];
            sipCall.ua = sipSdk.ua;
            sipCall.call = call;
            sipCall.cachedRemoteUri = remoteUri ?: @"";
            const char *cid = call_id(call);
            sipCall.cachedCallId = cid ? [NSString stringWithUTF8String:cid] : [[NSUUID UUID] UUIDString];
            sipCall.callUuid = [NSUUID UUID];
            sipCall.callHasVideo = call_has_video(call);
            sipCall.callIsOutgoing = call_is_outgoing(call);
            sipCall.callOnHold = call_is_onhold(call);
            cache[key] = sipCall;
        } else {
            sipCall.ua = sipSdk.ua;
            sipCall.call = call;
            if (remoteUri.length > 0) {
                sipCall.cachedRemoteUri = remoteUri;
            }
            sipCall.callHasVideo = call_has_video(call);
            sipCall.callOnHold = call_is_onhold(call);
        }
        return sipCall;
    }
}

static void invalidateSipCallsForClient(SipClient *sipSdk)
{
    NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
    @synchronized (cache) {
        for (NSValue *key in [cache.allKeys copy]) {
            SipCall *sipCall = cache[key];
            if (sipCall.ua == sipSdk.ua) {
                sipCall.call = nullptr;
                sipCall.ua = nullptr;
                [cache removeObjectForKey:key];
            }
        }
    }
}

// Internal layout matching libre's struct sdp_media (src/sdp/sdp.h)
struct sdp_media_internal {
    struct le le;
    struct list lfmtl;
    struct list rfmtl;
    struct list lattrl;
    struct list rattrl;
    struct sa laddr;
    struct sa raddr;
    struct sa laddr_rtcp;
    struct sa raddr_rtcp;
    int32_t lbwv[SDP_BANDWIDTH_MAX];
    int32_t rbwv[SDP_BANDWIDTH_MAX];
    char *name;
    char *proto;
    char *protov[8];
    char *uproto;
    sdp_media_enc_h *ench;
    void *arg;
    enum sdp_dir ldir;
    enum sdp_dir rdir;
    bool fmt_ignore;
    bool disabled;
    int dynpt;
};

static void preserveVideoRxAndDisplay(struct call *call, SipCall *sipCall)
{
    if (call == nullptr || sipCall == nil || !sipCall.callHasVideo) {
        return;
    }
    struct video *vid = call_video(call);
    if (!vid) return;
    struct stream *strm = video_strm(vid);
    if (!strm) return;
    struct sdp_media *m = stream_sdpmedia(strm);
    if (!m || sdp_media_disabled(m)) return;

    // Verify struct layout matches libre's internal sdp_media before writing
    auto *im = reinterpret_cast<struct sdp_media_internal *>(m);
    if (im->ldir == sdp_media_ldir(m) && im->rdir == sdp_media_rdir(m)) {
        im->rdir = SDP_SENDRECV;
    }

    (void)stream_enable_rx(strm, true);
    (void)video_start_display(vid, call_peeruri(call));
    video_req_keyframe(vid);
}

static void ua_event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
    SipClient *sipSdk = (__bridge SipClient *)arg;
    if (sipSdk == nil || event == nullptr) {
        return;
    }

    struct call *call = bevent_get_call(event);
    const char *peerUri = call ? call_peeruri(call) : nullptr;
    NSString *remoteUri = peerUri ? [NSString stringWithUTF8String:peerUri] : @"";
    SipCall *sipCall = getSipCall(call, sipSdk, remoteUri);
    const char *eventText = bevent_get_text(event);
    NSString *textParam = eventText ? [NSString stringWithUTF8String:eventText] : @"";

    NSLog(@"Baresip event %s (%d) peer=%@", bevent_str(ev), (int)ev, remoteUri);

    switch (ev) {
        case BEVENT_REGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onWillRegister:)]) {
                    [delegate onWillRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_REGISTER_OK: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onDidRegister:)]) {
                    [delegate onDidRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_REGISTER_FAIL: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onFailedRegister:)]) {
                    [delegate onFailedRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_UNREGISTERING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onWillUnRegister:)]) {
                    [delegate onWillUnRegister:sipSdk];
                }
            });
            break;
        }

        case BEVENT_CALL_INCOMING: {
            if (call) {
                sipCall.callHasVideo = call_has_video(call);
                sipCall.callIsOutgoing = NO;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallIncoming:)]) {
                    [delegate onCallIncoming:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_RINGING: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallRinging:)]) {
                    [delegate onCallRinging:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_PROGRESS: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallProcess:)]) {
                    [delegate onCallProcess:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_ESTABLISHED: {
            sipCall.establishedAt = [NSDate date];
            if (call) {
                sipCall.callHasVideo = call_has_video(call);
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallEstablished:)]) {
                    [delegate onCallEstablished:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_HOLD: {
            sipCall.callOnHold = YES;
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallHoldChanged:isOnHold:)]) {
                    [delegate onCallHoldChanged:sipCall isOnHold:YES];
                }
            });
            break;
        }

        case BEVENT_CALL_RESUME: {
            sipCall.callOnHold = NO;
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallHoldChanged:isOnHold:)]) {
                    [delegate onCallHoldChanged:sipCall isOnHold:NO];
                }
            });
            break;
        }

        case BEVENT_CALL_CLOSED: {
            sipCall.closed = YES;
            if (call != nullptr) {
                uint32_t dur = call_duration(call);
                if (dur > 0) {
                    sipCall.cachedDuration = dur;
                }
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                if (sipSdk.remoteVideoView != nil) {
                    [sipSdk.remoteVideoView clearVideo];
                }
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallClosed:)]) {
                    [delegate onCallClosed:sipCall];
                }
                [sipSdk releaseCallAudioSessionIfIdle];
            });

            if (call != nullptr) {
                NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
                @synchronized (cache) {
                    [cache removeObjectForKey:sipCallCacheKey(call)];
                }
            }
            sipCall.call = nullptr;
            sipCall.ua = nullptr;
            break;
        }

        case BEVENT_CALL_TRANSFER_FAILED: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallTransferFailed:)]) {
                    [delegate onCallTransferFailed:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_DTMF_START: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallDtmfStart:)]) {
                    [delegate onCallDtmfStart:sipCall];
                }
                if (textParam.length > 0 && [delegate respondsToSelector:@selector(onDtmfReceived:digit:)]) {
                    [delegate onDtmfReceived:sipCall digit:textParam];
                }
            });
            break;
        }

        case BEVENT_CALL_DTMF_END: {
            dispatch_async(dispatch_get_main_queue(), ^{
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallDtmfEnd:)]) {
                    [delegate onCallDtmfEnd:sipCall];
                }
            });
            break;
        }

        case BEVENT_CALL_REMOTE_SDP: {
            // Emitted right before call_update_media(call).
            // When remote answers an offer with recvonly, ensure rdir is restored
            // so call_update_media doesn't disable video RX or tear down vidisp.
            if (call != nullptr && [textParam isEqualToString:@"answer"] && sipCall.callHasVideo) {
                struct video *vid = call_video(call);
                struct stream *strm = vid ? video_strm(vid) : nullptr;
                struct sdp_media *m = strm ? stream_sdpmedia(strm) : nullptr;
                if (m && !sdp_media_disabled(m)) {
                    auto *im = reinterpret_cast<struct sdp_media_internal *>(m);
                    if (im->ldir == sdp_media_ldir(m) && im->rdir == sdp_media_rdir(m)) {
                        im->rdir = SDP_SENDRECV;
                    }
                }
            }
            break;
        }

        case BEVENT_CALL_LOCAL_SDP: {
            // Emitted right after sdp_encode encodes the local SDP.
            // When answering an in-dialog re-INVITE (e.g. from Asterisk PBX or peer
            // offering recvonly), sdp_encode has already written the RFC 3264 compliant
            // answer ("a=sendonly") into the response.
            // Now ensure video RX and vidisp display are restored and a keyframe requested
            // so incoming video RTP packets continue to be received and rendered without freeze.
            if (call != nullptr) {
                preserveVideoRxAndDisplay(call, sipCall);
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// MARK: - SipCall Implementation
// ============================================================================

@implementation SipCall

- (instancetype)init
{
    self = [super init];
    if (self) {
        _callUuid = [NSUUID UUID];
        _cachedRemoteUri = @"";
        _cachedCallId = _callUuid.UUIDString;
    }
    return self;
}

- (NSString *)remoteUri
{
    if (self.coreCall != nullptr) {
        const char *uri = sip_call_get_remote_uri(self.coreCall);
        if (uri && strlen(uri) > 0) {
            return [NSString stringWithUTF8String:uri];
        }
    }
    return self.cachedRemoteUri ?: @"";
}

- (NSString *)callId
{
    if (self.coreCall != nullptr) {
        const char *cid = sip_call_get_id(self.coreCall);
        if (cid && strlen(cid) > 0) {
            return [NSString stringWithUTF8String:cid];
        }
    }
    if (self.call != nullptr) {
        const char *cid = call_id(self.call);
        if (cid) {
            return [NSString stringWithUTF8String:cid];
        }
    }
    return self.cachedCallId ?: self.callUuid.UUIDString;
}

- (NSUUID *)uuid
{
    return self.callUuid;
}

- (BOOL)hasVideo
{
    if (self.coreCall != nullptr) {
        return sip_call_has_video(self.coreCall) || self.callHasVideo;
    }
    if (self.call != nullptr) {
        return call_has_video(self.call) || self.callHasVideo;
    }
    return self.callHasVideo;
}

- (BOOL)isOutgoing
{
    if (self.coreCall != nullptr) {
        return sip_call_is_outgoing(self.coreCall);
    }
    if (self.call != nullptr) {
        return call_is_outgoing(self.call);
    }
    return self.callIsOutgoing;
}

- (BOOL)isOnHold
{
    if (self.coreCall != nullptr) {
        return sip_call_is_on_hold(self.coreCall) || self.callOnHold;
    }
    if (self.call != nullptr) {
        return call_is_onhold(self.call) || self.callOnHold;
    }
    return self.callOnHold;
}

- (BOOL)isClosed
{
    if (self.coreCall != nullptr) {
        return sip_call_is_closed(self.coreCall) || _closed;
    }
    return _closed;
}

- (uint32_t)durationSeconds
{
    if (self.coreCall != nullptr) {
        uint32_t dur = sip_call_get_duration(self.coreCall);
        if (dur > 0) {
            return dur;
        }
    }
    if (self.call != nullptr) {
        uint32_t dur = call_duration(self.call);
        if (dur > 0) {
            return dur;
        }
    }
    if (self.cachedDuration > 0) {
        return self.cachedDuration;
    }
    if (self.establishedAt != nil) {
        NSTimeInterval elapsed = [[NSDate date] timeIntervalSinceDate:self.establishedAt];
        return elapsed > 0 ? (uint32_t)elapsed : 0;
    }
    return 0;
}

- (void)answer
{
    BOOL withVideo = self.hasVideo;
    if (gActiveSipClient != nil && !gActiveSipClient.videoEnabledByDefault) {
        withVideo = NO;
    }
    [self answerWithVideo:withVideo];
}

- (void)answerWithVideo:(BOOL)withVideo
{
    self.callHasVideo = withVideo;
    if (self.coreCall != nullptr) {
        sip_call_answer(self.coreCall, withVideo ? true : false);
        return;
    }
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.ua != nullptr && self.call != nullptr) {
            if (![gActiveSipClient prepareCallAudioSession]) {
                return;
            }
            self.callHasVideo = withVideo;
            (void)ua_answer(self.ua, self.call, withVideo ? VIDMODE_ON : VIDMODE_OFF);
        }
    });
}

- (void)hangup:(unsigned short)statusCode reason:(NSString *)reason
{
    self.closed = YES;
    if (self.coreCall != nullptr) {
        sip_call_hangup(self.coreCall, statusCode, reason.UTF8String ?: "");
        return;
    }
    NSString *reasonCopy = [reason copy];
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.ua != nullptr && self.call != nullptr) {
            const char *reasonText = reasonCopy.UTF8String ?: "";
            ua_hangup(self.ua, self.call, statusCode, reasonText);
        }
    });
}

- (void)holdAnswer
{
    BOOL nextHold = !self.isOnHold;
    (void)[self setHold:nextHold];
    if (self.coreCall != nullptr) {
        return;
    }
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.ua != nullptr && self.call != nullptr && !nextHold) {
            (void)ua_hold_answer(self.ua, self.call, self.hasVideo ? VIDMODE_ON : VIDMODE_OFF);
        }
    });
}

- (int)setHold:(BOOL)hold
{
    self.callOnHold = hold;
    if (self.coreCall != nullptr) {
        return sip_call_set_hold(self.coreCall, hold ? true : false);
    }
    if (self.call == nullptr) {
        return EINVAL;
    }
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.call != nullptr) {
            (void)call_hold(self.call, hold);
        }
    });
    return 0;
}

- (int)sendDtmf:(NSString *)digits
{
    if (digits.length == 0) {
        return EINVAL;
    }
    if (self.coreCall != nullptr) {
        return sip_call_send_dtmf(self.coreCall, digits.UTF8String ?: "");
    }
    if (self.call == nullptr) {
        return EINVAL;
    }
    NSString *digitsCopy = [digits copy];
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.call == nullptr) {
            return;
        }
        for (NSUInteger i = 0; i < digitsCopy.length; i++) {
            char ch = (char)[digitsCopy characterAtIndex:i];
            (void)call_send_digit(self.call, ch);
        }
        (void)call_send_digit(self.call, KEYCODE_REL);
    });
    return 0;
}

- (int)setVideoEnabled:(BOOL)enabled
{
    self.callHasVideo = enabled;
    if (gActiveSipClient != nil) {
        gActiveSipClient.localVideoEnabled = enabled;
    }
    return 0;
}

- (void)requestKeyframe
{
    if (self.coreCall != nullptr) {
        sip_call_request_keyframe(self.coreCall);
        return;
    }
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.call != nullptr) {
            struct video *vid = call_video(self.call);
            if (vid != nullptr) {
                video_req_keyframe(vid);
            }
        }
    });
}

@end

// ============================================================================
// MARK: - sip_abi Callbacks & Bridge
// ============================================================================

static void core_dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
{
    if (!task) return;
    if ([NSThread isMainThread]) {
        task(arg);
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            task(arg);
        });
    }
}

static void core_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data)
{
    if (!user_data) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    id<SipClientDelegate> delegate = sipClient.delegate;
    switch (state) {
        case SIP_REG_STATE_REGISTERING:
            if ([delegate respondsToSelector:@selector(onWillRegister:)]) {
                [delegate onWillRegister:sipClient];
            }
            break;
        case SIP_REG_STATE_REGISTERED:
            if ([delegate respondsToSelector:@selector(onDidRegister:)]) {
                [delegate onDidRegister:sipClient];
            }
            break;
        case SIP_REG_STATE_FAILED:
            if ([delegate respondsToSelector:@selector(onFailedRegister:)]) {
                [delegate onFailedRegister:sipClient];
            }
            break;
        case SIP_REG_STATE_UNREGISTERED:
            if ([delegate respondsToSelector:@selector(onWillUnRegister:)]) {
                [delegate onWillUnRegister:sipClient];
            }
            break;
    }
}

static void core_on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data)
{
    if (!user_data || !call) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    SipCall *sipCall = (__bridge SipCall *)sip_call_get_user_data(call);
    if (sipCall == nil) {
        sipCall = [[SipCall alloc] init];
        sipCall.coreCall = call;
        const char *remoteUri = sip_call_get_remote_uri(call);
        if (remoteUri) {
            sipCall.cachedRemoteUri = [NSString stringWithUTF8String:remoteUri];
        }
        const char *cid = sip_call_get_id(call);
        if (cid) {
            sipCall.cachedCallId = [NSString stringWithUTF8String:cid];
        }
        sipCall.callHasVideo = sip_call_has_video(call);
        sipCall.callIsOutgoing = sip_call_is_outgoing(call);
        sip_call_set_user_data(call, (__bridge void *)sipCall);
    }
    id<SipClientDelegate> delegate = sipClient.delegate;
    if ([delegate respondsToSelector:@selector(onCallIncoming:)]) {
        [delegate onCallIncoming:sipCall];
    }
}

static void core_on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
{
    if (!user_data || !call) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    SipCall *sipCall = (__bridge SipCall *)sip_call_get_user_data(call);
    if (sipCall == nil) {
        sipCall = [[SipCall alloc] init];
        sipCall.coreCall = call;
        const char *remoteUri = sip_call_get_remote_uri(call);
        if (remoteUri) {
            sipCall.cachedRemoteUri = [NSString stringWithUTF8String:remoteUri];
        }
        const char *cid = sip_call_get_id(call);
        if (cid) {
            sipCall.cachedCallId = [NSString stringWithUTF8String:cid];
        }
        sipCall.callHasVideo = sip_call_has_video(call);
        sipCall.callIsOutgoing = sip_call_is_outgoing(call);
        sip_call_set_user_data(call, (__bridge void *)sipCall);
    }
    id<SipClientDelegate> delegate = sipClient.delegate;
    switch (state) {
        case SIP_CALL_STATE_INCOMING:
            break;
        case SIP_CALL_STATE_CONNECTING:
            if ([delegate respondsToSelector:@selector(onCallProcess:)]) {
                [delegate onCallProcess:sipCall];
            }
            break;
        case SIP_CALL_STATE_RINGING:
            if ([delegate respondsToSelector:@selector(onCallRinging:)]) {
                [delegate onCallRinging:sipCall];
            }
            break;
        case SIP_CALL_STATE_ESTABLISHED:
            sipCall.establishedAt = [NSDate date];
            if ([delegate respondsToSelector:@selector(onCallEstablished:)]) {
                [delegate onCallEstablished:sipCall];
            }
            break;
        case SIP_CALL_STATE_HOLD:
            sipCall.callOnHold = sip_call_is_on_hold(call);
            if ([delegate respondsToSelector:@selector(onCallHoldChanged:isOnHold:)]) {
                [delegate onCallHoldChanged:sipCall isOnHold:sipCall.callOnHold];
            }
            break;
        case SIP_CALL_STATE_CLOSED:
            sipCall.closed = YES;
            sipCall.cachedDuration = sip_call_get_duration(call);
            if ([delegate respondsToSelector:@selector(onCallClosed:)]) {
                [delegate onCallClosed:sipCall];
            }
            break;
    }
}

static void core_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
{
    if (!user_data || !call) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    SipCall *sipCall = (__bridge SipCall *)sip_call_get_user_data(call);
    id<SipClientDelegate> delegate = sipClient.delegate;
    if (sipCall != nil) {
        sipCall.callOnHold = on_hold;
        if (width > 0 && height > 0) {
            if ([delegate respondsToSelector:@selector(onCallVideoSizeChanged:width:height:)]) {
                [delegate onCallVideoSizeChanged:sipCall width:width height:height];
            }
        }
        if ([delegate respondsToSelector:@selector(onCallHoldChanged:isOnHold:)]) {
            [delegate onCallHoldChanged:sipCall isOnHold:on_hold];
        }
    }
}

static void core_on_dtmf_digit(sip_call_t *call, const char *digit, void *user_data)
{
    if (!user_data || !call) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    SipCall *sipCall = (__bridge SipCall *)sip_call_get_user_data(call);
    id<SipClientDelegate> delegate = sipClient.delegate;
    NSString *dStr = digit ? [NSString stringWithUTF8String:digit] : @"";
    if (sipCall != nil) {
        if ([delegate respondsToSelector:@selector(onDtmfReceived:digit:)]) {
            [delegate onDtmfReceived:sipCall digit:dStr];
        }
        if ([delegate respondsToSelector:@selector(onCallDtmfStart:)]) {
            [delegate onCallDtmfStart:sipCall];
        }
        if ([delegate respondsToSelector:@selector(onCallDtmfEnd:)]) {
            [delegate onCallDtmfEnd:sipCall];
        }
    }
}

static bool core_prepare_audio_session(sip_client_t *client, void *user_data)
{
    if (!user_data) return true;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    return [sipClient prepareCallAudioSession] ? true : false;
}

static void core_release_audio_session(sip_client_t *client, void *user_data)
{
    if (!user_data) return;
    SipClient *sipClient = (__bridge SipClient *)user_data;
    [sipClient releaseCallAudioSessionIfIdle];
}

// ============================================================================
// MARK: - SipClient Implementation
// ============================================================================

@implementation SipClient

@synthesize localVideoEnabled = _localVideoEnabled;
@synthesize microphoneMuted = _microphoneMuted;

- (instancetype)initWithUsername:(NSString *)username domain:(NSString *)domain
{
    self = [super init];
    if (self) {
        _username = [username copy];
        _domain = [domain copy];
        _port = kDefaultPort;
        _transport = @"udp";
        _videoEnabledByDefault = YES;
        _localVideoEnabled = YES;
        _cameraPosition = SipCameraPositionFront;
        _microphoneMuted = NO;
        _speakerEnabled = NO;

        _captureDriver = [[IosVideoCaptureDriver alloc] initWithClient:self];
        _remoteRendererDriver = [[IosVideoRendererDriver alloc] initWithVideoView:nil];
        _localRendererDriver = [[IosVideoRendererDriver alloc] initWithVideoView:nil];
        _codecDriver = [[IosVideoCodecDriver alloc] init];

        sip_callbacks_t callbacks{};
        callbacks.dispatch_to_main = core_dispatch_to_main;
        callbacks.on_reg_state = core_on_reg_state;
        callbacks.on_incoming_call = core_on_incoming_call;
        callbacks.on_call_state = core_on_call_state;
        callbacks.on_call_media = core_on_call_media;
        callbacks.on_dtmf_digit = core_on_dtmf_digit;
        callbacks.prepare_audio_session = core_prepare_audio_session;
        callbacks.release_audio_session = core_release_audio_session;
        _coreClient = sip_client_create(&callbacks, (__bridge void *)self);
        [self registerNativeMediaDriversWithCore];
    }
    return self;
}

- (void)dealloc
{
    if (_coreClient != nullptr) {
        sip_client_destroy(_coreClient);
        _coreClient = nullptr;
    }
}

- (void)setRemoteVideoView:(SipVideoView *)remoteVideoView
{
    _remoteVideoView = remoteVideoView;
    _remoteRendererDriver.videoView = remoteVideoView;
}

- (void)setLocalVideoView:(SipVideoView *)localVideoView
{
    _localVideoView = localVideoView;
    _localRendererDriver.videoView = localVideoView;
}

- (void)setCameraPosition:(SipCameraPosition)cameraPosition
{
    _cameraPosition = cameraPosition;
    if (_coreClient != nullptr) {
        sip_client_set_camera(_coreClient, cameraPosition == SipCameraPositionFront ? 0 : 1);
    }
    [_captureDriver setCamera:(cameraPosition == SipCameraPositionFront ? 0 : 1)];
}

- (void)registerNativeMediaDriversWithCore
{
    if (_coreClient == nullptr) return;

    if (_codecDriver != nil) {
        sip_video_codec_driver_t cCodec = [_codecDriver cDriver];
        sip_client_set_video_codec_driver(_coreClient, &cCodec);
    }
    if (_captureDriver != nil) {
        sip_video_capture_driver_t cCap = [_captureDriver cDriver];
        sip_client_set_video_capture_driver(_coreClient, &cCap);
    }
    if (_remoteRendererDriver != nil) {
        sip_video_renderer_driver_t cRem = [_remoteRendererDriver cDriver];
        sip_client_set_remote_renderer(_coreClient, &cRem);
    }
    if (_localRendererDriver != nil) {
        sip_video_renderer_driver_t cLoc = [_localRendererDriver cDriver];
        sip_client_set_local_renderer(_coreClient, &cLoc);
    }
}

- (NSString *)aor
{
    if (self.ua == nullptr) {
        return @"";
    }

    struct account *account = ua_account(self.ua);
    const char *aor = account ? account_aor(account) : nullptr;
    return aor ? [NSString stringWithUTF8String:aor] : @"";
}

- (BOOL)prepareCallAudioSession
{
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *error = nil;
    AVAudioSessionCategoryOptions options = self.isSpeakerEnabled
        ? AVAudioSessionCategoryOptionDefaultToSpeaker : 0;
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                         mode:AVAudioSessionModeVoiceChat
                      options:options
                        error:&error] ||
        ![session setActive:YES error:&error]) {
        NSLog(@"Unable to prepare SIP call audio session: %@", error);
        return NO;
    }
    self.callAudioSessionActive = YES;
    return YES;
}

- (void)releaseCallAudioSessionIfIdle
{
    performSyncOnSipLoop(self, ^{
        if (!self.callAudioSessionActive) {
            return;
        }
        NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
        @synchronized (cache) {
            for (SipCall *call in cache.allValues) {
                if (call.ua == self.ua && call.call != nullptr) {
                    return;
                }
            }
        }
        NSError *error = nil;
        if ([AVAudioSession.sharedInstance setActive:NO
                                        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                              error:&error]) {
            self.callAudioSessionActive = NO;
        } else {
            NSLog(@"Unable to release SIP call audio session: %@", error);
        }
    });
}

- (void)registerNativeVideoPlugins
{
    if (self.mediaPluginsRegistered) {
        return;
    }

    [self registerNativeMediaDriversWithCore];

    vidcodec_register(baresip_vidcodecl(), &gH264CodecMode1);
    vidcodec_register(baresip_vidcodecl(), &gH264CodecMode0);

    (void)vidsrc_register(&gIosVidSrc,
                          baresip_vidsrcl(),
                          "avcapture",
                          ios_vidsrc_alloc,
                          ios_vidsrc_update);

    (void)vidisp_register(&gIosVidIsp,
                          baresip_vidispl(),
                          "iosview",
                          ios_vidisp_alloc,
                          ios_vidisp_update,
                          ios_vidisp_display,
                          ios_vidisp_hide);

    self.mediaPluginsRegistered = YES;
}

- (void)unregisterNativeVideoPlugins
{
    if (!self.mediaPluginsRegistered) {
        return;
    }

    vidcodec_unregister(&gH264CodecMode0);
    vidcodec_unregister(&gH264CodecMode1);
    gIosVidSrc = (struct vidsrc *)mem_deref(gIosVidSrc);
    gIosVidIsp = (struct vidisp *)mem_deref(gIosVidIsp);
    self.mediaPluginsRegistered = NO;
}

- (int)start
{
    @synchronized (self) {
        if (self.stackStarted) {
            return 0;
        }

        gActiveSipClient = self;
        self.startResult = 0;
        self.loopStartedSemaphore = dispatch_semaphore_create(0);
        self.loopStoppedSemaphore = dispatch_semaphore_create(0);
        self.loopThread = [[NSThread alloc] initWithTarget:self
                                                  selector:@selector(runMainLoop)
                                                    object:nil];
        self.loopThread.name = @"SipSdk Baresip main loop";
        self.loopThreadStarted = YES;
        [self.loopThread start];

        dispatch_semaphore_wait(self.loopStartedSemaphore, DISPATCH_TIME_FOREVER);
        if (self.startResult != 0) {
            dispatch_semaphore_wait(self.loopStoppedSemaphore, DISPATCH_TIME_FOREVER);
            self.loopThreadStarted = NO;
            self.loopThread = nil;
            self.loopStartedSemaphore = nil;
            self.loopStoppedSemaphore = nil;
            return self.startResult;
        }

        return 0;
    }
}

- (int)initializeEngineOnLoopThread
{
    int result = libre_init();
    if (result != 0) {
        return result;
    }

    NSString *documentsRoot =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                            NSUserDomainMask,
                                            YES).firstObject;
    if (documentsRoot == nil) {
        return EIO;
    }

    NSString *documentDirectory =
        [documentsRoot stringByAppendingPathComponent:@"baresip-v4"];
    NSFileManager *fileManager = [NSFileManager defaultManager];
    NSError *directoryError = nil;
    if (![fileManager createDirectoryAtPath:documentDirectory
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:&directoryError]) {
        NSLog(@"Failed to create Baresip v4 config directory: %@", directoryError);
        return directoryError ? (int)directoryError.code : EIO;
    }

    NSString *configDestination =
        [documentDirectory stringByAppendingPathComponent:@"config"];
    NSBundle *mainBundle = [NSBundle mainBundle];
    NSString *bundledConfigPath =
        [mainBundle pathForResource:@"config" ofType:@"baresip-v4"];
    if (bundledConfigPath == nil) {
        NSArray<NSURL *> *resourceBundleURLs =
            [mainBundle URLsForResourcesWithExtension:@"bundle"
                                         subdirectory:nil];
        for (NSURL *resourceBundleURL in resourceBundleURLs) {
            NSBundle *resourceBundle =
                [NSBundle bundleWithURL:resourceBundleURL];
            bundledConfigPath =
                [resourceBundle pathForResource:@"config"
                                          ofType:@"baresip-v4"];
            if (bundledConfigPath != nil) {
                break;
            }
        }
    }
    if (bundledConfigPath == nil) {
        NSLog(@"Baresip v4 config resource is missing from the app bundle.");
        return ENOENT;
    }

    if ([fileManager fileExistsAtPath:configDestination]) {
        [fileManager removeItemAtPath:configDestination error:nil];
    }
    NSError *copyError = nil;
    if (![fileManager copyItemAtPath:bundledConfigPath
                              toPath:configDestination
                               error:&copyError]) {
        NSLog(@"Failed to copy Baresip v4 config into Documents: %@", copyError);
        return copyError ? (int)copyError.code : EIO;
    }

    result = conf_path_set(documentDirectory.UTF8String);
    if (result != 0) {
        return result;
    }

    result = conf_configure();
    if (result != 0) {
        return result;
    }

    struct config *cfg = conf_config();
    if (cfg != nullptr) {
        snprintf(cfg->video.src_mod, sizeof(cfg->video.src_mod), "%s", "avcapture");
        snprintf(cfg->video.src_dev, sizeof(cfg->video.src_dev), "%s",
                 self.cameraPosition == SipCameraPositionBack ? "back" : "front");
        snprintf(cfg->video.disp_mod, sizeof(cfg->video.disp_mod), "%s", "iosview");
        if (cfg->video.width == 0 || cfg->video.height == 0) {
            cfg->video.width = 640;
            cfg->video.height = 480;
        }
        if (cfg->video.fps <= 0) {
            cfg->video.fps = 30.0;
        }
        if (cfg->video.bitrate == 0) {
            cfg->video.bitrate = 512000;
        }
        cfg->video.enc_fmt = VID_FMT_YUV420P;
    }

    re_thread_async_init(kAsyncWorkerCount);

    result = baresip_init(conf_config());
    if (result != 0) {
        return result;
    }

    // Ensure 127.0.0.1 is available alongside hardware interfaces for local SIP tests
    if (baresip_network() != nullptr) {
        struct sa loAddr;
        if (sa_set_str(&loAddr, "127.0.0.1", 0) == 0 && !net_is_laddr(baresip_network(), &loAddr)) {
            (void)net_add_address_ifname(baresip_network(), &loAddr, "lo0");
        }
        struct sa dnsServers[2];
        if (sa_set_str(&dnsServers[0], "8.8.8.8", 53) == 0 &&
            sa_set_str(&dnsServers[1], "1.1.1.1", 53) == 0) {
            (void)net_use_nameserver(baresip_network(), dnsServers, 2);
        }
    }

    // Register VideoToolbox H264, avcapture vidsrc, and iosview vidisp before conf_modules()
    // so account.so and ua_alloc both see the registered H264 video codecs.
    [self registerNativeVideoPlugins];

    result = ua_init("SipSdk", true, true, false);
    if (result != 0) {
        return result;
    }

    result = bevent_register(ua_event_handler, (__bridge void *)self);
    if (result != 0) {
        return result;
    }
    self.eventsRegistered = YES;

    result = conf_modules();
    if (result != 0) {
        return result;
    }

    NSString *transportParam = @"";
    if ([self.transport.lowercaseString isEqualToString:@"tcp"]) {
        transportParam = @";transport=tcp";
    }
    NSString *aor = [NSString stringWithFormat:@"sip:%@@%@:%d%@",
                     self.username, self.domain, self.port, transportParam];
    result = ua_alloc(&_ua, aor.UTF8String);
    if (result != 0 || self.ua == nullptr) {
        return result != 0 ? result : EIO;
    }

    struct account *account = ua_account(self.ua);
    if (account == nullptr) {
        return EIO;
    }

    if (self.displayName.length > 0) {
        (void)account_set_display_name(account, self.displayName.UTF8String);
    }

    result = account_set_auth_user(account, self.username.UTF8String);
    if (result != 0) {
        return result;
    }

    if (self.password.length > 0) {
        result = account_set_auth_pass(account, self.password.UTF8String);
        if (result != 0) {
            return result;
        }
    }

    (void)account_set_video_codecs(account, "H264");

    result = mqueue_alloc(&_controlMq, control_mqueue_handler, (__bridge void *)self);
    if (result != 0) {
        return result;
    }

    self.stackStarted = YES;
    return 0;
}

- (void)runMainLoop
{
    @autoreleasepool {
        int rc = [self initializeEngineOnLoopThread];
        self.startResult = rc;
        dispatch_semaphore_signal(self.loopStartedSemaphore);

        if (rc == 0) {
            (void)re_main(nullptr);
        }

        [self cleanupEngine];
        dispatch_semaphore_signal(self.loopStoppedSemaphore);
    }
}

- (void)cleanupEngine
{
    if (self.controlMq != nullptr) {
        self.controlMq = (struct mqueue *)mem_deref(self.controlMq);
    }

    if (self.eventsRegistered) {
        bevent_unregister(ua_event_handler);
        self.eventsRegistered = NO;
    }

    invalidateSipCallsForClient(self);
    self.ua = nullptr;

    ua_close();
    [self releaseCallAudioSessionIfIdle];
    [self unregisterNativeVideoPlugins];
    module_app_unload();
    conf_close();
    baresip_close();
    mod_close();
    re_thread_async_close();
    libre_close();

    self.stackStarted = NO;
}

- (void)stop
{
    @synchronized (self) {
        if (!self.stackStarted && !self.loopThreadStarted) {
            return;
        }

        if (self.loopThreadStarted) {
            if (self.controlMq != nullptr) {
                (void)mqueue_push(self.controlMq, 1, nullptr);
            } else {
                re_cancel();
            }
            dispatch_semaphore_wait(self.loopStoppedSemaphore, DISPATCH_TIME_FOREVER);
        }

        self.loopThreadStarted = NO;
        self.loopThread = nil;
        self.loopStartedSemaphore = nil;
        self.loopStoppedSemaphore = nil;
    }
}

- (int)registry
{
    if (_coreClient != nullptr) {
        (void)sip_client_register(_coreClient,
                                  self.username.UTF8String,
                                  self.domain.UTF8String,
                                  self.password.UTF8String ?: "",
                                  self.transport.UTF8String ?: "udp",
                                  self.port > 0 ? self.port : 5060);
    }
    if (self.ua == nullptr) {
        return (_coreClient != nullptr) ? 0 : EINVAL;
    }
    __block int rc = 0;
    performSyncOnSipLoop(self, ^{
        rc = (self.ua != nullptr) ? ua_register(self.ua) : EINVAL;
    });
    return rc;
}

- (void)unregister
{
    if (_coreClient != nullptr) {
        sip_client_unregister(_coreClient);
    }
    performAsyncOnSipLoop(self, ^{
        if (self.ua != nullptr) {
            ua_unregister(self.ua);
        }
    });
}

- (bool)isRegistered
{
    if (_coreClient != nullptr && sip_client_is_registered(_coreClient)) {
        return true;
    }
    return self.ua != nullptr && ua_isregistered(self.ua);
}

- (SipCall *)makeCall:(NSString *)uri
{
    return [self makeCall:uri withVideo:self.videoEnabledByDefault];
}

- (SipCall *)makeCall:(NSString *)uri withVideo:(BOOL)withVideo
{
    if (self.ua == nullptr || uri.length == 0) {
        NSLog(@"Cannot place a SIP call until the client is started and URI is non-empty.");
        return nil;
    }

    NSString *dialUri = [uri copy];
    if (![dialUri hasPrefix:@"sip:"]) {
        if (![dialUri containsString:@"@"] && self.domain.length > 0) {
            dialUri = [NSString stringWithFormat:@"sip:%@@%@", dialUri, self.domain];
        } else {
            dialUri = [NSString stringWithFormat:@"sip:%@", dialUri];
        }
    } else if (![dialUri containsString:@"@"] && self.domain.length > 0) {
        dialUri = [NSString stringWithFormat:@"%@@%@", dialUri, self.domain];
    }

    __block struct call *call = nullptr;
    __block int result = 0;
    enum vidmode vmode = withVideo ? VIDMODE_ON : VIDMODE_OFF;
    performSyncOnSipLoop(self, ^{
        if (self.ua != nullptr) {
            result = [self prepareCallAudioSession]
                ? ua_connect(self.ua, &call, nullptr, dialUri.UTF8String, vmode)
                : EIO;
            if (result != 0) {
                [self releaseCallAudioSessionIfIdle];
            }
        } else {
            result = EINVAL;
        }
    });
    if (result != 0 || call == nullptr) {
        NSLog(@"Failed to create SIP call to '%@' (result %d).", dialUri, result);
        return nil;
    }

    const char *peerUri = call_peeruri(call);
    NSString *remoteUri = peerUri ? [NSString stringWithUTF8String:peerUri] : dialUri;
    SipCall *sipCall = getSipCall(call, self, remoteUri);
    sipCall.callHasVideo = withVideo;
    sipCall.callIsOutgoing = YES;

    if (_coreClient != nullptr) {
        sip_call_t *cCall = sip_call_make(_coreClient, dialUri.UTF8String, withVideo);
        if (cCall != nullptr) {
            sipCall.coreCall = cCall;
            sip_call_set_user_data(cCall, (__bridge void *)sipCall);
        }
    }

    return sipCall;
}

- (void)setMuted:(BOOL)muted
{
    _microphoneMuted = muted;
    if (_coreClient != nullptr) {
        sip_client_set_mic_muted(_coreClient, muted ? true : false);
    }
    performAsyncOnSipLoop(self, ^{
        NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
        @synchronized (cache) {
            for (SipCall *sipCall in cache.allValues) {
                if (sipCall.coreCall != nullptr) {
                    sip_call_set_muted(sipCall.coreCall, muted ? true : false);
                }
                if (sipCall.call != nullptr) {
                    struct audio *au = call_audio(sipCall.call);
                    if (au != nullptr) {
                        audio_mute(au, muted);
                    }
                }
            }
        }
    });
}

- (BOOL)isMicrophoneMuted
{
    if (_coreClient != nullptr) {
        return sip_client_is_mic_muted(_coreClient) || _microphoneMuted;
    }
    return _microphoneMuted;
}

- (void)setSpeakerphone:(BOOL)enabled
{
    _speakerEnabled = enabled;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *err = nil;
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
             withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker
                   error:&err];
    [session overrideOutputAudioPort:(enabled ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone)
                               error:&err];
}

- (void)switchCamera
{
    self.cameraPosition = (self.cameraPosition == SipCameraPositionFront)
        ? SipCameraPositionBack
        : SipCameraPositionFront;

    if (_coreClient != nullptr) {
        sip_client_switch_camera(_coreClient);
    }
    if (_captureDriver != nil) {
        [_captureDriver switchCamera];
    }

    const char *dev = (self.cameraPosition == SipCameraPositionBack) ? "back" : "front";
    NSString *nsDev = [NSString stringWithUTF8String:dev];

    if (conf_config() != nullptr) {
        snprintf(conf_config()->video.src_dev, sizeof(conf_config()->video.src_dev), "%s", dev);
    }

    SipAVCaptureHelper *activeHelper = gActiveCaptureHelper;
    if (activeHelper != nil) {
        [activeHelper switchToDeviceName:nsDev];
    }

    performAsyncOnSipLoop(self, ^{
        NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
        @synchronized (cache) {
            for (SipCall *sipCall in cache.allValues) {
                if (sipCall.call != nullptr) {
                    struct video *vid = call_video(sipCall.call);
                    if (vid != nullptr) {
                        video_vidsrc_set_device(vid, dev);
                    }
                }
            }
        }
    });
}

- (void)setLocalVideoEnabled:(BOOL)localVideoEnabled
{
    _localVideoEnabled = localVideoEnabled;
    if (_coreClient != nullptr) {
        sip_client_set_local_video_enabled(_coreClient, localVideoEnabled ? true : false);
    }
    [_captureDriver setEnabled:localVideoEnabled];
    if (!localVideoEnabled && self.localVideoView != nil) {
        [self.localVideoView clearVideo];
    }
}

- (BOOL)isLocalVideoEnabled
{
    if (_coreClient != nullptr) {
        return sip_client_is_local_video_enabled(_coreClient);
    }
    return _localVideoEnabled;
}

- (void)startLocalVideoPreview
{
    if (self.localPreviewHelper != nil) {
        return;
    }
    if (gActiveSipClient == nil) {
        gActiveSipClient = self;
    }
    if (_coreClient != nullptr) {
        sip_client_start_local_preview(_coreClient);
    }
    if (_captureDriver != nil) {
        [_captureDriver startWithWidth:640 height:480 fps:30];
    }
    SipAVCaptureHelper *helper = [[SipAVCaptureHelper alloc]
        initWithOwner:NULL
                width:640
               height:480
                  fps:30.0];
    self.localPreviewHelper = helper;
    NSString *deviceName = (self.cameraPosition == SipCameraPositionBack) ? @"back" : @"front";
    [helper startWithDeviceName:deviceName];
}

- (void)stopLocalVideoPreview
{
    if (_coreClient != nullptr) {
        sip_client_stop_local_preview(_coreClient);
    }
    if (_captureDriver != nil) {
        [_captureDriver stop];
    }
    SipAVCaptureHelper *helper = self.localPreviewHelper;
    self.localPreviewHelper = nil;
    [helper stop];
}

- (BOOL)isLocalVideoPreviewActive
{
    if (_coreClient != nullptr && sip_client_is_local_preview_active(_coreClient)) {
        return YES;
    }
    return self.localPreviewHelper != nil;
}

- (BOOL)isLiveCameraFeedActive
{
    return self.localPreviewHelper.captureSession.isRunning;
}

- (BOOL)hasVideoCodecRegistered
{
    if (!self.stackStarted) {
        return NO;
    }
    const struct vidcodec *enc = vidcodec_find_encoder(baresip_vidcodecl(), "H264");
    const struct vidcodec *dec = vidcodec_find_decoder(baresip_vidcodecl(), "H264");
    return enc != nullptr && dec != nullptr;
}

- (BOOL)hasVideoSourceRegistered
{
    if (!self.stackStarted) {
        return NO;
    }
    return vidsrc_find(baresip_vidsrcl(), "avcapture") != nullptr;
}

- (BOOL)hasVideoDisplayRegistered
{
    if (!self.stackStarted) {
        return NO;
    }
    return vidisp_find(baresip_vidispl(), "iosview") != nullptr;
}

struct H264RoundTripPacket {
    bool marker;
    uint64_t rtp_ts;
    std::vector<uint8_t> payload;
};

static std::vector<H264RoundTripPacket> *gRoundTripPackets = nullptr;

static int roundTripPacketHandler(bool marker,
                                  uint64_t rtp_ts,
                                  const uint8_t *hdr,
                                  size_t hdr_len,
                                  const uint8_t *pld,
                                  size_t pld_len,
                                  const struct video *vid)
{
    (void)vid;
    if (!gRoundTripPackets) {
        return 0;
    }
    H264RoundTripPacket pkt;
    pkt.marker = marker;
    pkt.rtp_ts = rtp_ts;
    if (hdr && hdr_len > 0) {
        pkt.payload.insert(pkt.payload.end(), hdr, hdr + hdr_len);
    }
    if (pld && pld_len > 0) {
        pkt.payload.insert(pkt.payload.end(), pld, pld + pld_len);
    }
    gRoundTripPackets->push_back(std::move(pkt));
    return 0;
}

+ (BOOL)testH264RoundTripWithWidth:(int)width height:(int)height
{
    if (width <= 0 || height <= 0) {
        return NO;
    }

    struct videnc_param encPrm = {
        .bitrate = 512000,
        .pktsize = 1200,
        .fps = 30.0,
        .max_fs = 0,
    };

    struct videnc_state *ves = nullptr;
    if (h264_enc_update(&ves, &gH264CodecMode1, &encPrm, gH264CodecMode1.fmtp, roundTripPacketHandler, nullptr) != 0 || !ves) {
        return NO;
    }

    struct viddec_state *vds = nullptr;
    if (h264_dec_update(&vds, &gH264CodecMode1, gH264CodecMode1.fmtp, nullptr) != 0 || !vds) {
        mem_deref(ves);
        return NO;
    }

    std::vector<H264RoundTripPacket> packets;
    gRoundTripPackets = &packets;

    struct vidsz sz = { (unsigned)width, (unsigned)height };
    size_t yuvSize = vidframe_size(VID_FMT_YUV420P, &sz);
    std::vector<uint8_t> yuvBuf(yuvSize, 0);

    uint8_t *yPlane = yuvBuf.data();
    uint8_t *uPlane = yPlane + (size_t)width * (size_t)height;
    uint8_t *vPlane = uPlane + (size_t)(width / 2) * (size_t)(height / 2);
    fillSyntheticYUV420Pattern(yPlane, uPlane, vPlane, width, height, 1);

    struct vidframe srcFrame;
    vidframe_init_buf(&srcFrame, VID_FMT_YUV420P, &sz, yuvBuf.data());

    int encErr = h264_enc_encode(ves, true, &srcFrame, 1000000ULL);
    gRoundTripPackets = nullptr;

    if (encErr != 0 || packets.empty()) {
        mem_deref(vds);
        mem_deref(ves);
        return NO;
    }

    struct vidframe dstFrame;
    memset(&dstFrame, 0, sizeof(dstFrame));
    uint16_t seq = 1;

    for (const auto &rtpPkt : packets) {
        struct mbuf *mb = mbuf_alloc(rtpPkt.payload.size());
        if (!mb) {
            continue;
        }
        mbuf_write_mem(mb, rtpPkt.payload.data(), rtpPkt.payload.size());
        mb->pos = 0;

        struct rtp_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.m = rtpPkt.marker;
        hdr.seq = seq++;
        hdr.ts = (uint32_t)rtpPkt.rtp_ts;

        struct viddec_packet decPkt = {
            .intra = false,
            .hdr = &hdr,
            .timestamp = 1000000ULL,
            .mb = mb,
        };
        (void)h264_dec_decode(vds, &dstFrame, &decPkt);
        mem_deref(mb);
    }

    BOOL ok = vidframe_isvalid(&dstFrame) &&
              dstFrame.size.w == (unsigned)width &&
              dstFrame.size.h == (unsigned)height;

    mem_deref(vds);
    mem_deref(ves);
    return ok;
}

+ (BOOL)testH264PacketLossRecoveryWithWidth:(int)width height:(int)height
{
    struct videnc_param encPrm = { .bitrate = 512000, .pktsize = 1200, .fps = 30.0, .max_fs = 0 };
    struct videnc_state *ves = nullptr;
    if (h264_enc_update(&ves, &gH264CodecMode1, &encPrm, gH264CodecMode1.fmtp, roundTripPacketHandler, nullptr) != 0 || !ves) {
        return NO;
    }
    struct viddec_state *vds = nullptr;
    if (h264_dec_update(&vds, &gH264CodecMode1, gH264CodecMode1.fmtp, nullptr) != 0 || !vds) {
        mem_deref(ves);
        return NO;
    }

    struct vidsz sz = { (unsigned)width, (unsigned)height };
    std::vector<uint8_t> yuvBuf(vidframe_size(VID_FMT_YUV420P, &sz), 0);
    uint8_t *yPlane = yuvBuf.data();
    uint8_t *uPlane = yPlane + (size_t)width * (size_t)height;
    uint8_t *vPlane = uPlane + (size_t)(width / 2) * (size_t)(height / 2);

    // Frames: 0 = IDR, 1..5 = P, 6 = forced IDR. Packet loss hits frame 2.
    const int frameCount = 7;
    const int lossFrame = 2;
    const int idrFrame = 6;
    std::vector<std::vector<H264RoundTripPacket>> frames(frameCount);
    for (int i = 0; i < frameCount; i++) {
        fillSyntheticYUV420Pattern(yPlane, uPlane, vPlane, width, height, i + 1);
        struct vidframe srcFrame;
        vidframe_init_buf(&srcFrame, VID_FMT_YUV420P, &sz, yuvBuf.data());
        gRoundTripPackets = &frames[i];
        int err = h264_enc_encode(ves, i == idrFrame, &srcFrame, 1000000ULL + (uint64_t)i * 33333ULL);
        gRoundTripPackets = nullptr;
        if (err != 0 || frames[i].empty()) {
            mem_deref(vds);
            mem_deref(ves);
            return NO;
        }
    }

    BOOL ok = YES;
    uint16_t seq = 1;
    for (int i = 0; i < frameCount; i++) {
        struct vidframe dstFrame;
        memset(&dstFrame, 0, sizeof(dstFrame));
        int markerErr = 0;
        for (size_t p = 0; p < frames[i].size(); p++) {
            const auto &rtpPkt = frames[i][p];
            uint16_t pktSeq = seq++;
            if (i == lossFrame && p == 0) {
                continue; // lost on the network
            }
            struct mbuf *mb = mbuf_alloc(rtpPkt.payload.size());
            if (!mb) {
                ok = NO;
                continue;
            }
            mbuf_write_mem(mb, rtpPkt.payload.data(), rtpPkt.payload.size());
            mb->pos = 0;
            struct rtp_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.m = rtpPkt.marker;
            hdr.seq = pktSeq;
            hdr.ts = (uint32_t)rtpPkt.rtp_ts;
            struct viddec_packet decPkt = { .intra = false, .hdr = &hdr, .timestamp = 1000000ULL, .mb = mb };
            int err = h264_dec_decode(vds, &dstFrame, &decPkt);
            if (rtpPkt.marker) {
                markerErr = err;
            }
            mem_deref(mb);
        }
        BOOL shown = vidframe_isvalid(&dstFrame);
        if (i < lossFrame && (!shown || markerErr != 0)) ok = NO;
        if (i == lossFrame && markerErr == 0) ok = NO;   // loss must be reported
        if (i >= lossFrame && i < idrFrame && shown) ok = NO; // no broken frames
        if (i == idrFrame && (!shown || markerErr != 0)) ok = NO; // recovers
        NSLog(@"[h264-loss-test] frame %d packets=%zu err=%d shown=%d", i, frames[i].size(), markerErr, shown);
    }

    mem_deref(vds);
    mem_deref(ves);
    return ok;
}

+ (BOOL)sendH264TestFrameToPort:(uint16_t)port
                           ssrc:(uint32_t)ssrc
                            seq:(uint16_t *)seq
                             ts:(uint32_t)ts
                          width:(int)width
                         height:(int)height
                            idr:(BOOL)idr
{
    if (port == 0 || seq == nullptr) return NO;

    struct videnc_param encPrm = { .bitrate = 512000, .pktsize = 1200, .fps = 30.0, .max_fs = 0 };
    struct videnc_state *ves = nullptr;
    if (h264_enc_update(&ves, &gH264CodecMode1, &encPrm, gH264CodecMode1.fmtp, roundTripPacketHandler, nullptr) != 0 || !ves) {
        return NO;
    }

    struct vidsz sz = { (unsigned)width, (unsigned)height };
    std::vector<uint8_t> yuvBuf(vidframe_size(VID_FMT_YUV420P, &sz), 0);
    uint8_t *yPlane = yuvBuf.data();
    uint8_t *uPlane = yPlane + (size_t)width * (size_t)height;
    uint8_t *vPlane = uPlane + (size_t)(width / 2) * (size_t)(height / 2);
    fillSyntheticYUV420Pattern(yPlane, uPlane, vPlane, width, height, (int)(ts & 0xff));

    struct vidframe srcFrame;
    vidframe_init_buf(&srcFrame, VID_FMT_YUV420P, &sz, yuvBuf.data());

    std::vector<H264RoundTripPacket> pkts;
    gRoundTripPackets = &pkts;
    int err = h264_enc_encode(ves, idr, &srcFrame, (uint64_t)ts);
    gRoundTripPackets = nullptr;
    mem_deref(ves);

    if (err != 0 || pkts.empty()) {
        return NO;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NO;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (const auto &p : pkts) {
        uint8_t rtpPkt[1500];
        if (12 + p.payload.size() > sizeof(rtpPkt)) continue;
        rtpPkt[0] = 0x80;
        rtpPkt[1] = (p.marker ? 0x80 : 0x00) | (96 & 0x7f);
        uint16_t s = (*seq)++;
        rtpPkt[2] = (s >> 8) & 0xff;
        rtpPkt[3] = s & 0xff;
        rtpPkt[4] = (ts >> 24) & 0xff;
        rtpPkt[5] = (ts >> 16) & 0xff;
        rtpPkt[6] = (ts >> 8) & 0xff;
        rtpPkt[7] = ts & 0xff;
        rtpPkt[8] = (ssrc >> 24) & 0xff;
        rtpPkt[9] = (ssrc >> 16) & 0xff;
        rtpPkt[10] = (ssrc >> 8) & 0xff;
        rtpPkt[11] = ssrc & 0xff;
        memcpy(rtpPkt + 12, p.payload.data(), p.payload.size());

        sendto(sock, rtpPkt, 12 + p.payload.size(), 0, (struct sockaddr *)&dst, sizeof(dst));
    }

    close(sock);
    return YES;
}

@end
