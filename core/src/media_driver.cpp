#include "media_driver.h"
#include "engine.h"

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#endif

namespace sip_core {

// ============================================================================
// Synthetic YUV420P Frame Generator
// ============================================================================

static void fill_synthetic_yuv420(uint8_t *y_plane,
                                  uint8_t *u_plane,
                                  uint8_t *v_plane,
                                  int width,
                                  int height,
                                  uint32_t tick)
{
    static const uint8_t kBarY[8] = {235, 210, 170, 145, 106,  81,  41,  24};
    static const uint8_t kBarU[8] = {128,  16, 166,  54, 202,  90, 240, 128};
    static const uint8_t kBarV[8] = {128, 146,  16,  34, 222, 240, 110, 128};

    int bar_width = width / 8;
    if (bar_width < 1) bar_width = 1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int bar_idx = (x / bar_width) & 7;
            y_plane[y * width + x] = kBarY[bar_idx];
        }
    }

    int uv_w = width / 2;
    int uv_h = height / 2;
    for (int y = 0; y < uv_h; ++y) {
        for (int x = 0; x < uv_w; ++x) {
            int bar_idx = ((x * 2) / bar_width) & 7;
            u_plane[y * uv_w + x] = kBarU[bar_idx];
            v_plane[y * uv_w + x] = kBarV[bar_idx];
        }
    }

    int box_w = width / 8;
    int box_h = height / 8;
    if (box_w < 8) box_w = 8;
    if (box_h < 8) box_h = 8;
    int max_x = width - box_w;
    int max_y = height - box_h;
    if (max_x > 0 && max_y > 0) {
        int box_x = (int)((tick * 6) % (unsigned)max_x);
        int box_y = (int)((tick * 3) % (unsigned)max_y);
        for (int y = box_y; y < box_y + box_h && y < height; ++y) {
            for (int x = box_x; x < box_x + box_w && x < width; ++x) {
                y_plane[y * width + x] = 235;
                if ((y / 2) < uv_h && (x / 2) < uv_w) {
                    u_plane[(y / 2) * uv_w + (x / 2)] = 180;
                    v_plane[(y / 2) * uv_w + (x / 2)] = 80;
                }
            }
        }
    }
}

struct SyntheticVideoCaptureDriver::Impl {
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{true};
    std::atomic<int> camera_position{0};
    int width{640};
    int height{480};
    int fps{30};
    FrameHandler handler;
    std::mutex mutex;
    std::thread thread;
    uint32_t tick{0};

    void loop() {
        int sleep_ms = (fps > 0) ? (1000 / fps) : 33;
        if (sleep_ms < 5) sleep_ms = 5;

        size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
        size_t uv_size = (static_cast<size_t>(width) / 2) * (static_cast<size_t>(height) / 2);
        std::vector<uint8_t> buffer(y_size + uv_size * 2, 0);

        uint8_t *y_plane = buffer.data();
        uint8_t *u_plane = y_plane + y_size;
        uint8_t *v_plane = u_plane + uv_size;

        while (running) {
            auto start_time = std::chrono::steady_clock::now();
            if (enabled.load()) {
                fill_synthetic_yuv420(y_plane, u_plane, v_plane, width, height, ++tick);
            } else {
                std::memset(y_plane, 16, y_size);
                std::memset(u_plane, 128, uv_size);
                std::memset(v_plane, 128, uv_size);
            }

            FrameHandler current_handler;
            {
                std::lock_guard<std::mutex> lock(mutex);
                current_handler = handler;
            }

            if (current_handler) {
                VideoFrame vf;
                vf.width = width;
                vf.height = height;
                vf.y = y_plane;
                vf.u = u_plane;
                vf.v = v_plane;
                vf.y_stride = width;
                vf.u_stride = width / 2;
                vf.v_stride = width / 2;
                vf.timestamp_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        start_time.time_since_epoch()).count());
                current_handler(vf);
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            long wait_ms = sleep_ms - elapsed;
            if (wait_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            }
        }
    }
};

SyntheticVideoCaptureDriver::SyntheticVideoCaptureDriver()
    : m_impl(new Impl()) {}

SyntheticVideoCaptureDriver::~SyntheticVideoCaptureDriver()
{
    stop();
    delete m_impl;
}

int SyntheticVideoCaptureDriver::start(int width, int height, int fps)
{
    stop();
    m_impl->width = (width > 0) ? width : 640;
    m_impl->height = (height > 0) ? height : 480;
    m_impl->fps = (fps > 0) ? fps : 30;
    m_impl->running = true;
    m_impl->thread = std::thread(&Impl::loop, m_impl);
    return 0;
}

void SyntheticVideoCaptureDriver::stop()
{
    if (m_impl->running.exchange(false)) {
        if (m_impl->thread.joinable()) {
            m_impl->thread.join();
        }
    }
}

void SyntheticVideoCaptureDriver::set_frame_handler(FrameHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->handler = std::move(handler);
}

void SyntheticVideoCaptureDriver::switch_camera()
{
    m_impl->camera_position = (m_impl->camera_position.load() == 0) ? 1 : 0;
}

void SyntheticVideoCaptureDriver::set_camera(int position)
{
    m_impl->camera_position = position;
}

void SyntheticVideoCaptureDriver::set_enabled(bool enabled)
{
    m_impl->enabled = enabled;
}

bool SyntheticVideoCaptureDriver::is_enabled() const
{
    return m_impl->enabled.load();
}

// ============================================================================
// C ABI Adapters
// ============================================================================

CApiVideoCaptureDriver::CApiVideoCaptureDriver(const sip_video_capture_driver_t &driver)
    : m_driver(driver) {}

CApiVideoCaptureDriver::~CApiVideoCaptureDriver()
{
    stop();
}

int CApiVideoCaptureDriver::start(int width, int height, int fps)
{
    return m_driver.start ? m_driver.start(m_driver.user_data, width, height, fps) : 0;
}

void CApiVideoCaptureDriver::stop()
{
    if (m_driver.stop) {
        m_driver.stop(m_driver.user_data);
    }
}

void CApiVideoCaptureDriver::set_frame_handler(FrameHandler handler)
{
    m_cpp_handler = std::move(handler);
    if (m_driver.set_frame_handler) {
        if (m_cpp_handler) {
            m_driver.set_frame_handler(m_driver.user_data, c_frame_callback, this);
        } else {
            m_driver.set_frame_handler(m_driver.user_data, nullptr, nullptr);
        }
    }
}

void CApiVideoCaptureDriver::switch_camera()
{
    if (m_driver.switch_camera) {
        m_driver.switch_camera(m_driver.user_data);
    }
}

void CApiVideoCaptureDriver::set_camera(int position)
{
    if (m_driver.set_camera) {
        m_driver.set_camera(m_driver.user_data, position);
    }
}

void CApiVideoCaptureDriver::set_enabled(bool enabled)
{
    if (m_driver.set_enabled) {
        m_driver.set_enabled(m_driver.user_data, enabled);
    }
}

bool CApiVideoCaptureDriver::is_enabled() const
{
    return m_driver.is_enabled ? m_driver.is_enabled(m_driver.user_data) : true;
}

void CApiVideoCaptureDriver::c_frame_callback(const sip_video_frame_t *frame, void *arg)
{
    auto *self = static_cast<CApiVideoCaptureDriver *>(arg);
    if (self && self->m_cpp_handler && frame) {
        VideoFrame vf;
        vf.width = frame->width;
        vf.height = frame->height;
        vf.y = frame->y;
        vf.u = frame->u;
        vf.v = frame->v;
        vf.y_stride = frame->y_stride;
        vf.u_stride = frame->u_stride;
        vf.v_stride = frame->v_stride;
        vf.timestamp_ns = frame->timestamp_ns;
        self->m_cpp_handler(vf);
    }
}

CApiVideoRendererDriver::CApiVideoRendererDriver(const sip_video_renderer_driver_t &driver)
    : m_driver(driver) {}

CApiVideoRendererDriver::~CApiVideoRendererDriver() = default;

void CApiVideoRendererDriver::render_frame(const VideoFrame &frame)
{
    if (m_driver.render_frame) {
        sip_video_frame_t c_frame{};
        c_frame.width = frame.width;
        c_frame.height = frame.height;
        c_frame.y = frame.y;
        c_frame.u = frame.u;
        c_frame.v = frame.v;
        c_frame.y_stride = frame.y_stride;
        c_frame.u_stride = frame.u_stride;
        c_frame.v_stride = frame.v_stride;
        c_frame.timestamp_ns = frame.timestamp_ns;
        m_driver.render_frame(m_driver.user_data, &c_frame);
    }
}

void CApiVideoRendererDriver::on_stream_resized(int width, int height)
{
    if (m_driver.on_stream_resized) {
        m_driver.on_stream_resized(m_driver.user_data, width, height);
    }
}

void CApiVideoRendererDriver::clear()
{
    if (m_driver.clear) {
        m_driver.clear(m_driver.user_data);
    }
}

CApiVideoCodecDriver::CApiVideoCodecDriver(const sip_video_codec_driver_t &driver)
    : m_driver(driver) {}

CApiVideoCodecDriver::~CApiVideoCodecDriver() = default;

int CApiVideoCodecDriver::register_codec()
{
    return m_driver.register_codec ? m_driver.register_codec(m_driver.user_data) : 0;
}

void CApiVideoCodecDriver::unregister_codec()
{
    if (m_driver.unregister_codec) {
        m_driver.unregister_codec(m_driver.user_data);
    }
}

} // namespace sip_core

// ============================================================================
// Global Scope Baresip Plugin Hooks & Codec State
// ============================================================================

static sip_core::MediaDriverManager *s_active_manager = nullptr;

#if defined(__APPLE__)

constexpr size_t kDefaultRtpPayloadSize = 1200;

struct EncodedRtpPacket {
    bool marker{false};
    uint64_t rtp_ts{0};
    std::vector<uint8_t> hdr;
    std::vector<uint8_t> pld;
};

struct videnc_state {
    VTCompressionSessionRef session{nullptr};
    unsigned width{0};
    unsigned height{0};
    struct videnc_param prm{};
    videnc_packet_h *pkth{nullptr};
    const struct video *vid{nullptr};
    uint64_t current_rtp_ts{0};
    uint32_t frame_index{0};
    std::mutex pkt_mutex;
    std::vector<EncodedRtpPacket> pending_packets;
};

struct viddec_state {
    VTDecompressionSessionRef session{nullptr};
    CMVideoFormatDescriptionRef formatDesc{nullptr};
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> format_sps;
    std::vector<uint8_t> format_pps;
    std::vector<uint8_t> au_buffer;
    std::vector<uint8_t> fu_buffer;
    bool fu_active{false};
    bool au_is_idr{false};
    std::mutex decode_mutex;
    std::vector<uint8_t> decoded_yuv;
    unsigned decoded_w{0};
    unsigned decoded_h{0};
    bool has_decoded_frame{false};
    bool have_seq{false};
    uint16_t last_seq{0};
    bool have_au_ts{false};
    uint32_t au_ts{0};
    bool au_corrupt{false};
    bool need_idr{true};
    bool decode_failed{false};
    uint64_t last_picup_ms{0};
};

static void sendRfc6184Nal(struct videnc_state *ves,
                           const uint8_t *nal,
                           size_t nalLen,
                           bool isLastNalOfAccessUnit)
{
    if (!ves || !ves->pkth || !nal || nalLen == 0) return;

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
    (void)sourceFrameRefCon;
    (void)infoFlags;
    if (status != noErr || !sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer)) return;

    auto *ves = static_cast<struct videnc_state *>(outputCallbackRefCon);
    if (!ves || !ves->pkth) return;

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
    if (!block) return;

    size_t totalLen = 0;
    char *dataPtr = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &totalLen, &dataPtr) != noErr || !dataPtr) return;

    size_t offset = 0;
    while (offset + 4 <= totalLen) {
        uint32_t nalLen = 0;
        std::memcpy(&nalLen, dataPtr + offset, 4);
        nalLen = CFSwapInt32BigToHost(nalLen);
        offset += 4;
        if (nalLen == 0 || offset + nalLen > totalLen) break;
        bool isLast = (offset + nalLen >= totalLen);
        sendRfc6184Nal(ves, reinterpret_cast<const uint8_t *>(dataPtr + offset), nalLen, isLast);
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
    if (!vesp || !prm || !pkth) return EINVAL;

    struct videnc_state *ves = *vesp;
    if (!ves) {
        void *raw = mem_zalloc(sizeof(struct videnc_state), h264_enc_destructor);
        if (!raw) return ENOMEM;
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
    if (status != noErr || !ves->session) return EIO;

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
    if (!ves || !frame || !vidframe_isvalid(frame)) return EINVAL;

    unsigned width = frame->size.w;
    unsigned height = frame->size.h;
    if (width == 0 || height == 0) return EINVAL;

    int err = ensureEncoderSession(ves, width, height);
    if (err != 0) return err;

    CVPixelBufferRef pixelBuffer = nullptr;
    CVReturn cvErr = CVPixelBufferCreate(
        kCFAllocatorDefault,
        width,
        height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        nullptr,
        &pixelBuffer);
    if (cvErr != kCVReturnSuccess || !pixelBuffer) return EIO;

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    uint8_t *dstY = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
    size_t dstYStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    uint8_t *dstUV = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
    size_t dstUVStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

    if (frame->fmt == VID_FMT_YUV420P) {
        for (unsigned y = 0; y < height; y++) {
            std::memcpy(dstY + y * dstYStride, frame->data[0] + y * frame->linesize[0], width);
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
    if (frameProps) CFRelease(frameProps);
    CVPixelBufferRelease(pixelBuffer);

    if (status != noErr) return EIO;

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
    (void)sourceFrameRefCon;
    (void)infoFlags;
    (void)presentationTimeStamp;
    (void)presentationDuration;

    auto *vds = static_cast<struct viddec_state *>(decompressionOutputRefCon);
    if (!vds) return;
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
            std::memcpy(dstY + y * width, srcY + y * srcYStride, width);
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
    if (!vdsp) return EINVAL;

    struct viddec_state *vds = *vdsp;
    if (!vds) {
        void *raw = mem_zalloc(sizeof(struct viddec_state), h264_dec_destructor);
        if (!raw) return ENOMEM;
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
        vds->need_idr = true;
        vds->decode_failed = false;
        vds->last_picup_ms = 0;
        *vdsp = vds;
    }
    return 0;
}

static void appendNalToAccessUnit(struct viddec_state *vds, const uint8_t *nal, size_t len)
{
    if (!vds || !nal || len == 0) return;
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
    if (vds->sps.empty() || vds->pps.empty()) return EINVAL;

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
    if (status != noErr || !vds->formatDesc) return EIO;

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

    if (status != noErr || !vds->session) return EIO;

    vds->format_sps = vds->sps;
    vds->format_pps = vds->pps;
    return 0;
}

static int h264_dec_decode(struct viddec_state *vds,
                           struct vidframe *frame,
                           struct viddec_packet *pkt)
{
    if (!vds || !frame || !pkt || !pkt->mb) return EINVAL;

    const uint8_t *buf = mbuf_buf(pkt->mb);
    size_t len = mbuf_get_left(pkt->mb);
    if (!buf || len == 0) return 0;

    if (pkt->hdr) {
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
            if (naluSize == 0 || offset + naluSize > len) break;
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
            vds->au_corrupt = true;
        }

        if (endBit && vds->fu_active) {
            appendNalToAccessUnit(vds, vds->fu_buffer.data(), vds->fu_buffer.size());
            vds->fu_buffer.clear();
            vds->fu_active = false;
        }
    }

    bool marker = (pkt->hdr != nullptr) ? pkt->hdr->m : false;
    if (!marker) return 0;

    bool corrupt = vds->au_corrupt;
    vds->au_corrupt = false;
    if (corrupt) vds->need_idr = true;

    if (vds->au_buffer.empty() && !vds->need_idr) return 0;

    pkt->intra = vds->au_is_idr;
    if (vds->au_is_idr && !corrupt) vds->need_idr = false;
    if (vds->need_idr) {
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
    LE_INIT,
    "96",
    "H264",
    "packetization-mode=1",
    "packetization-mode=1;profile-level-id=42e01f",
    h264_enc_update,
    h264_enc_encode,
    h264_dec_update,
    h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};

static struct vidcodec gH264CodecMode0 = {
    LE_INIT,
    "97",
    "H264",
    "packetization-mode=0",
    "packetization-mode=0;profile-level-id=42e01f",
    h264_enc_update,
    h264_enc_encode,
    h264_dec_update,
    h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};

#endif

// ============================================================================
// vidsrc & vidisp Baresip Plugin Structs & Hooks
// ============================================================================

struct vidsrc_st {
    const struct vidsrc *vs{nullptr};
    vidsrc_frame_h *frameh{nullptr};
    void *arg{nullptr};
    struct vidsz size{0, 0};
    double fps{30.0};
    std::shared_ptr<sip_core::IVideoCaptureDriver> driver;
    sip_core::MediaDriverManager *manager{nullptr};
};

static void core_vidsrc_destructor(void *arg)
{
    auto *st = static_cast<struct vidsrc_st *>(arg);
    if (st && st->manager) {
        st->manager->on_vidsrc_destroyed();
    }
    if (st && st->driver) {
        if (!st->manager || !st->manager->is_local_preview_active()) {
            st->driver->stop();
            st->driver->set_frame_handler(nullptr);
        }
        st->driver.reset();
    }
}

static int core_vidsrc_alloc(struct vidsrc_st **vsp,
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
    (void)dev;
    (void)packeth;
    (void)errorh;
    if (!vsp || !size || !frameh) return EINVAL;

    auto *manager = s_active_manager;
    if (!manager) return EIO;

    auto *st = static_cast<struct vidsrc_st *>(mem_zalloc(sizeof(struct vidsrc_st), core_vidsrc_destructor));
    if (!st) return ENOMEM;

    st->vs = vs;
    st->frameh = frameh;
    st->arg = arg;
    st->size = *size;
    st->fps = (prm && prm->fps > 0) ? prm->fps : 30.0;
    st->manager = manager;

    auto driver = manager->capture_driver();
    st->driver = driver;

    if (driver) {
        driver->set_frame_handler([st, manager](const sip_core::VideoFrame &frame) {
            if (!st) return;

            auto local_renderer = manager->local_renderer();
            if (local_renderer && st->driver && st->driver->is_enabled() && manager->is_local_video_enabled()) {
                local_renderer->render_frame(frame);
            }

            if (!st->driver || !st->driver->is_enabled() || !manager->is_local_video_enabled() || !st->frameh) return;

            struct vidsz sz = { static_cast<unsigned>(frame.width), static_cast<unsigned>(frame.height) };
            struct vidframe vf;
            std::memset(&vf, 0, sizeof(vf));
            vf.size = sz;
            vf.fmt = VID_FMT_YUV420P;
            vf.data[0] = const_cast<uint8_t *>(frame.y);
            vf.data[1] = const_cast<uint8_t *>(frame.u);
            vf.data[2] = const_cast<uint8_t *>(frame.v);
            vf.linesize[0] = static_cast<uint16_t>(frame.y_stride > 0 ? frame.y_stride : frame.width);
            vf.linesize[1] = static_cast<uint16_t>(frame.u_stride > 0 ? frame.u_stride : (frame.width / 2));
            vf.linesize[2] = static_cast<uint16_t>(frame.v_stride > 0 ? frame.v_stride : (frame.width / 2));

            uint64_t ts = frame.timestamp_ns > 0 ? (frame.timestamp_ns / 1000ULL) : (tmr_jiffies() * 1000ULL);
            st->frameh(&vf, ts, st->arg);
        });

        int rc = driver->start(static_cast<int>(size->w), static_cast<int>(size->h), static_cast<int>(st->fps));
        if (rc != 0) {
            mem_deref(st);
            return rc;
        }
    }

    manager->on_vidsrc_allocated();
    *vsp = st;
    return 0;
}

static void core_vidsrc_update(struct vidsrc_st *st,
                               struct vidsrc_prm *prm,
                               const char *dev)
{
    (void)prm;
    if (st && st->driver && dev) {
        if (std::strcmp(dev, "back") == 0) {
            st->driver->set_camera(1);
        } else {
            st->driver->set_camera(0);
        }
    }
}

struct vidisp_st {
    const struct vidisp *vd{nullptr};
    vidisp_resize_h *resizeh{nullptr};
    void *arg{nullptr};
    struct vidsz last_size{0, 0};
    sip_core::MediaDriverManager *manager{nullptr};
};

static void core_vidisp_destructor(void *arg)
{
    (void)arg;
}

static int core_vidisp_alloc(struct vidisp_st **vp,
                             const struct vidisp *vd,
                             struct vidisp_prm *prm,
                             const char *dev,
                             vidisp_resize_h *resizeh,
                             void *arg)
{
    (void)prm;
    (void)dev;
    if (!vp) return EINVAL;

    auto *manager = s_active_manager;
    if (!manager) return EIO;

    auto *st = static_cast<struct vidisp_st *>(mem_zalloc(sizeof(struct vidisp_st), core_vidisp_destructor));
    if (!st) return ENOMEM;

    st->vd = vd;
    st->resizeh = resizeh;
    st->arg = arg;
    st->last_size = {0, 0};
    st->manager = manager;

    *vp = st;
    return 0;
}

static int core_vidisp_update(struct vidisp_st *st,
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

static int core_vidisp_display(struct vidisp_st *st,
                               const char *title,
                               const struct vidframe *frame,
                               uint64_t timestamp)
{
    (void)title;
    if (!st || !frame || !vidframe_isvalid(frame)) return EINVAL;

    unsigned w = frame->size.w;
    unsigned h = frame->size.h;
    if (w == 0 || h == 0) return EINVAL;

    if (st->last_size.w != w || st->last_size.h != h) {
        st->last_size = frame->size;
        if (st->resizeh) {
            st->resizeh(&frame->size, st->arg);
        }
        if (st->manager) {
            st->manager->handle_stream_resized(static_cast<int>(w), static_cast<int>(h));
        }
    }

    if (st->manager) {
        st->manager->handle_display_frame(frame, timestamp);
    }

    return 0;
}

static void core_vidisp_hide(struct vidisp_st *st)
{
    if (st && st->manager) {
        st->manager->handle_display_hide();
    }
}

// ============================================================================
// MediaDriverManager Implementation
// ============================================================================

namespace sip_core {

#if defined(__APPLE__)
class AppleVideoToolboxCodecDriver : public IVideoCodecDriver {
public:
    int register_codec() override {
        vidcodec_register(baresip_vidcodecl(), &gH264CodecMode1);
        vidcodec_register(baresip_vidcodecl(), &gH264CodecMode0);
        return 0;
    }

    void unregister_codec() override {
        vidcodec_unregister(&gH264CodecMode0);
        vidcodec_unregister(&gH264CodecMode1);
    }
};
#else
class MockH264CodecDriver : public IVideoCodecDriver {
public:
    int register_codec() override { return 0; }
    void unregister_codec() override {}
};
#endif

MediaDriverManager::MediaDriverManager(Engine *engine)
    : m_engine(engine),
      m_default_synthetic_driver(std::make_shared<SyntheticVideoCaptureDriver>())
{
#if defined(__APPLE__)
    m_default_codec_driver = std::make_shared<AppleVideoToolboxCodecDriver>();
#else
    m_default_codec_driver = std::make_shared<MockH264CodecDriver>();
#endif
}

MediaDriverManager::~MediaDriverManager()
{
    unregister_baresip_plugins();
}

MediaDriverManager *MediaDriverManager::active_manager()
{
    return s_active_manager;
}

void MediaDriverManager::set_capture_driver(std::shared_ptr<IVideoCaptureDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_user_capture_driver = driver;
}

void MediaDriverManager::set_remote_renderer(std::shared_ptr<IVideoRendererDriver> renderer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_remote_renderer = renderer;
}

void MediaDriverManager::set_local_renderer(std::shared_ptr<IVideoRendererDriver> renderer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_local_renderer = renderer;
}

void MediaDriverManager::set_codec_driver(std::shared_ptr<IVideoCodecDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_plugins_registered) {
        auto old_codec = m_codec_driver ? m_codec_driver : m_default_codec_driver;
        if (old_codec) {
            old_codec->unregister_codec();
        }
        m_codec_driver = driver;
        auto new_codec = m_codec_driver ? m_codec_driver : m_default_codec_driver;
        if (new_codec) {
            new_codec->register_codec();
        }
    } else {
        m_codec_driver = driver;
    }
}

std::shared_ptr<IVideoCaptureDriver> MediaDriverManager::capture_driver()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_user_capture_driver) {
        return m_user_capture_driver;
    }
    return m_default_synthetic_driver;
}

std::shared_ptr<IVideoRendererDriver> MediaDriverManager::remote_renderer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_remote_renderer;
}

std::shared_ptr<IVideoRendererDriver> MediaDriverManager::local_renderer()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_local_renderer;
}

std::shared_ptr<IVideoCodecDriver> MediaDriverManager::codec_driver()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_codec_driver ? m_codec_driver : m_default_codec_driver;
}

int MediaDriverManager::register_baresip_plugins()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_plugins_registered) {
        return 0;
    }

    s_active_manager = this;

    auto codec = m_codec_driver ? m_codec_driver : m_default_codec_driver;
    if (codec) {
        codec->register_codec();
    }

    int rc = vidsrc_register(&m_vidsrc,
                             baresip_vidsrcl(),
                             "avcapture",
                             core_vidsrc_alloc,
                             core_vidsrc_update);
    if (rc != 0) {
        if (codec) codec->unregister_codec();
        s_active_manager = nullptr;
        return rc;
    }

    rc = vidisp_register(&m_vidisp,
                         baresip_vidispl(),
                         "iosview",
                         core_vidisp_alloc,
                         core_vidisp_update,
                         core_vidisp_display,
                         core_vidisp_hide);
    if (rc != 0) {
        m_vidsrc = static_cast<struct vidsrc *>(mem_deref(m_vidsrc));
        if (codec) codec->unregister_codec();
        s_active_manager = nullptr;
        return rc;
    }

    m_plugins_registered = true;
    return 0;
}

void MediaDriverManager::unregister_baresip_plugins()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_plugins_registered) {
        return;
    }

    auto codec = m_codec_driver ? m_codec_driver : m_default_codec_driver;
    if (codec) {
        codec->unregister_codec();
    }

    if (m_vidsrc) {
        m_vidsrc = static_cast<struct vidsrc *>(mem_deref(m_vidsrc));
    }
    if (m_vidisp) {
        m_vidisp = static_cast<struct vidisp *>(mem_deref(m_vidisp));
    }

    if (s_active_manager == this) {
        s_active_manager = nullptr;
    }

    m_plugins_registered = false;
}

void MediaDriverManager::handle_display_frame(const struct vidframe *frame, uint64_t timestamp)
{
    if (!frame) return;
    auto renderer = remote_renderer();
    if (!renderer) return;

    VideoFrame vf;
    vf.width = static_cast<int>(frame->size.w);
    vf.height = static_cast<int>(frame->size.h);
    vf.y = frame->data[0];
    vf.u = frame->data[1];
    vf.v = frame->data[2];
    vf.y_stride = frame->linesize[0];
    vf.u_stride = frame->linesize[1];
    vf.v_stride = frame->linesize[2];
    vf.timestamp_ns = timestamp * 1000ULL;

    renderer->render_frame(vf);
}

void MediaDriverManager::handle_stream_resized(int width, int height)
{
    auto renderer = remote_renderer();
    if (renderer) {
        renderer->on_stream_resized(width, height);
    }
    if (m_engine) {
        m_engine->notify_media_resize(width, height);
    }
}

void MediaDriverManager::handle_display_hide()
{
    auto renderer = remote_renderer();
    if (renderer) {
        renderer->clear();
    }
}

int MediaDriverManager::switch_camera()
{
    m_camera_position = (m_camera_position.load() == 0) ? 1 : 0;
    auto driver = capture_driver();
    if (driver) {
        driver->switch_camera();
    }
    return 0;
}

int MediaDriverManager::set_camera(int position)
{
    m_camera_position.store(position);
    auto driver = capture_driver();
    if (driver) {
        driver->set_camera(position);
    }
    return 0;
}

int MediaDriverManager::set_local_video_enabled(bool enabled)
{
    m_local_video_enabled.store(enabled);
    auto driver = capture_driver();
    if (driver) {
        driver->set_enabled(enabled);
    }
    if (!enabled) {
        auto renderer = local_renderer();
        if (renderer) {
            renderer->clear();
        }
    }
    return 0;
}

bool MediaDriverManager::is_local_video_enabled() const
{
    auto driver = const_cast<MediaDriverManager *>(this)->capture_driver();
    if (driver) {
        return driver->is_enabled() && m_local_video_enabled.load();
    }
    return m_local_video_enabled.load();
}

int MediaDriverManager::start_local_preview()
{
    m_local_preview_active.store(true);
    auto driver = capture_driver();
    if (driver) {
        driver->set_frame_handler([this, driver](const VideoFrame &frame) {
            if (!driver || !driver->is_enabled() || !m_local_video_enabled.load()) {
                return;
            }
            auto renderer = local_renderer();
            if (renderer) {
                renderer->render_frame(frame);
            }
        });
        return driver->start(640, 480, 30);
    }
    return 0;
}

int MediaDriverManager::stop_local_preview()
{
    m_local_preview_active.store(false);
    if (m_active_vidsrc_count.load() == 0) {
        auto driver = capture_driver();
        if (driver) {
            driver->stop();
            driver->set_frame_handler(nullptr);
        }
    }
    auto renderer = local_renderer();
    if (renderer) {
        renderer->clear();
    }
    return 0;
}

bool MediaDriverManager::is_local_preview_active() const
{
    return m_local_preview_active.load();
}

void MediaDriverManager::on_vidsrc_allocated()
{
    m_active_vidsrc_count.fetch_add(1, std::memory_order_relaxed);
}

void MediaDriverManager::on_vidsrc_destroyed()
{
    if (m_active_vidsrc_count.fetch_sub(1, std::memory_order_acq_rel) <= 1) {
        if (m_local_preview_active.load()) {
            auto driver = capture_driver();
            if (driver) {
                driver->set_frame_handler([this, driver](const VideoFrame &frame) {
                    if (!driver || !driver->is_enabled() || !m_local_video_enabled.load()) {
                        return;
                    }
                    auto renderer = local_renderer();
                    if (renderer) {
                        renderer->render_frame(frame);
                    }
                });
            }
        }
    }
}

} // namespace sip_core
