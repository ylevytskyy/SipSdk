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
// MARK: - Private Interfaces & Active Client State
// ============================================================================

@interface SipCall ()
@property (nonatomic) struct ua *ua;
@property (nonatomic) struct call *call;
@property (nonatomic, copy) NSString *cachedRemoteUri;
@property (nonatomic, copy) NSString *cachedCallId;
@property (nonatomic, strong) NSUUID *callUuid;
@property (nonatomic, assign) BOOL callHasVideo;
@property (nonatomic, assign) BOOL callIsOutgoing;
@property (nonatomic, assign) BOOL callOnHold;
@property (nonatomic, strong) NSDate *establishedAt;
@end

@interface SipClient ()
@property (nonatomic) struct ua *ua;
@property (nonatomic) struct mqueue *controlMq;
@property (nonatomic, strong) NSThread *loopThread;
@property (nonatomic, strong) dispatch_semaphore_t loopStartedSemaphore;
@property (nonatomic, strong) dispatch_semaphore_t loopStoppedSemaphore;
@property (nonatomic) int startResult;
@property (nonatomic) BOOL loopThreadStarted;
@property (nonatomic) BOOL stackStarted;
@property (nonatomic) BOOL eventsRegistered;
@property (nonatomic) BOOL mediaPluginsRegistered;
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
    if (status != noErr || !imageBuffer) {
        return;
    }

    auto *vds = static_cast<struct viddec_state *>(decompressionOutputRefCon);
    if (!vds) {
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

    if (vds->au_buffer.empty()) {
        return 0;
    }

    pkt->intra = vds->au_is_idr;

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
                vds->has_decoded_frame = false;
                VTDecodeInfoFlags flagOut = 0;
                VTDecompressionSessionDecodeFrame(
                    vds->session,
                    sampleBuffer,
                    0,
                    nullptr,
                    &flagOut);
                VTDecompressionSessionWaitForAsynchronousFrames(vds->session);
                CFRelease(sampleBuffer);

                std::lock_guard<std::mutex> lock(vds->decode_mutex);
                if (vds->has_decoded_frame && vds->decoded_w > 0 && vds->decoded_h > 0) {
                    struct vidsz sz = { vds->decoded_w, vds->decoded_h };
                    vidframe_init_buf(frame, VID_FMT_YUV420P, &sz, vds->decoded_yuv.data());
                }
            }
            CFRelease(blockBuffer);
        }
    }

    vds->au_buffer.clear();
    vds->au_is_idr = false;
    return 0;
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
    if (!st || !st->frameh) {
        return;
    }

    SipClient *client = gActiveSipClient;
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
    if (!st || !st->frameh) {
        return;
    }

    SipClient *client = gActiveSipClient;
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
    dispatch_async(self.captureQueue, ^{
        [self stopInternal];
    });
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
    }

    if (client.remoteVideoView != nil && frame->fmt == VID_FMT_YUV420P) {
        [client.remoteVideoView renderYUV420WithY:frame->data[0]
                                                u:frame->data[1]
                                                v:frame->data[2]
                                          yStride:frame->linesize[0]
                                          uStride:frame->linesize[1]
                                          vStride:frame->linesize[2]
                                            width:(int)w
                                           height:(int)h];
    }
    return 0;
}

static void ios_vidisp_hide(struct vidisp_st *st)
{
    (void)st;
    SipClient *client = gActiveSipClient;
    if (client.remoteVideoView != nil) {
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
            dispatch_async(dispatch_get_main_queue(), ^{
                if (sipSdk.remoteVideoView != nil) {
                    [sipSdk.remoteVideoView clearVideo];
                }
                id<SipClientDelegate> delegate = sipSdk.delegate;
                if ([delegate respondsToSelector:@selector(onCallClosed:)]) {
                    [delegate onCallClosed:sipCall];
                }
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
    return self.cachedRemoteUri ?: @"";
}

- (NSString *)callId
{
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
    if (self.call != nullptr) {
        return call_has_video(self.call) || self.callHasVideo;
    }
    return self.callHasVideo;
}

- (BOOL)isOutgoing
{
    if (self.call != nullptr) {
        return call_is_outgoing(self.call);
    }
    return self.callIsOutgoing;
}

- (BOOL)isOnHold
{
    if (self.call != nullptr) {
        return call_is_onhold(self.call) || self.callOnHold;
    }
    return self.callOnHold;
}

- (uint32_t)durationSeconds
{
    if (self.call != nullptr) {
        uint32_t dur = call_duration(self.call);
        if (dur > 0) {
            return dur;
        }
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
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.ua != nullptr && self.call != nullptr) {
            self.callHasVideo = withVideo;
            (void)ua_answer(self.ua, self.call, withVideo ? VIDMODE_ON : VIDMODE_OFF);
        }
    });
}

- (void)hangup:(unsigned short)statusCode reason:(NSString *)reason
{
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
    performAsyncOnSipLoop(gActiveSipClient, ^{
        if (self.ua != nullptr && self.call != nullptr && !nextHold) {
            (void)ua_hold_answer(self.ua, self.call, self.hasVideo ? VIDMODE_ON : VIDMODE_OFF);
        }
    });
}

- (int)setHold:(BOOL)hold
{
    self.callOnHold = hold;
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
    if (self.call == nullptr || digits.length == 0) {
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
// MARK: - SipClient Implementation
// ============================================================================

@implementation SipClient

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
    }
    return self;
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

- (void)registerNativeVideoPlugins
{
    if (self.mediaPluginsRegistered) {
        return;
    }

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
    if (self.ua == nullptr) {
        return EINVAL;
    }
    __block int rc = 0;
    performSyncOnSipLoop(self, ^{
        rc = (self.ua != nullptr) ? ua_register(self.ua) : EINVAL;
    });
    return rc;
}

- (void)unregister
{
    performSyncOnSipLoop(self, ^{
        if (self.ua != nullptr) {
            ua_unregister(self.ua);
        }
    });
}

- (bool)isRegistered
{
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

    NSString *uriCopy = [uri copy];
    __block struct call *call = nullptr;
    __block int result = 0;
    enum vidmode vmode = withVideo ? VIDMODE_ON : VIDMODE_OFF;
    performSyncOnSipLoop(self, ^{
        if (self.ua != nullptr) {
            result = ua_connect(self.ua, &call, nullptr, uriCopy.UTF8String, vmode);
        } else {
            result = EINVAL;
        }
    });
    if (result != 0 || call == nullptr) {
        NSLog(@"Failed to create SIP call (result %d).", result);
        return nil;
    }

    const char *peerUri = call_peeruri(call);
    NSString *remoteUri = peerUri ? [NSString stringWithUTF8String:peerUri] : uriCopy;
    SipCall *sipCall = getSipCall(call, self, remoteUri);
    sipCall.callHasVideo = withVideo;
    sipCall.callIsOutgoing = YES;
    return sipCall;
}

- (void)setMuted:(BOOL)muted
{
    _microphoneMuted = muted;
    performAsyncOnSipLoop(self, ^{
        NSMutableDictionary<NSValue *, SipCall *> *cache = sipCallsCache();
        @synchronized (cache) {
            for (SipCall *sipCall in cache.allValues) {
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
    if (!localVideoEnabled && self.localVideoView != nil) {
        [self.localVideoView clearVideo];
    }
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

@end
