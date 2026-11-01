#include "media_codec_driver.h"
#include "media_driver.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <android/log.h>
#include <dlfcn.h>

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#define TAG "SipMediaCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

constexpr size_t kDefaultRtpPayloadSize = 1200;

struct EncodedRtpPacket {
    bool marker{false};
    uint64_t rtp_ts{0};
    std::vector<uint8_t> hdr;
    std::vector<uint8_t> pld;
};

// Global remote window reference
static std::mutex g_window_mutex;
static ANativeWindow *g_remote_window = nullptr;

struct videnc_state {
    AMediaCodec *encoder{nullptr};
    unsigned width{0};
    unsigned height{0};
    int color_format{21}; // 21: COLOR_FormatYUV420SemiPlanar (NV12)
    struct videnc_param prm{};
    videnc_packet_h *pkth{nullptr};
    const struct video *vid{nullptr};
    uint64_t current_rtp_ts{0};
    uint32_t frame_index{0};
    std::mutex pkt_mutex;
    std::vector<EncodedRtpPacket> pending_packets;
};

struct viddec_state {
    AMediaCodec *decoder{nullptr};
    ANativeWindow *current_window{nullptr};
    unsigned width{0};
    unsigned height{0};
    std::vector<uint8_t> au_buffer;
    bool au_is_idr{false};
    std::vector<uint8_t> fu_buffer;
    bool fu_active{false};
    bool have_seq{false};
    uint16_t last_seq{0};
    bool have_au_ts{false};
    uint32_t au_ts{0};
    bool au_corrupt{false};
    bool need_idr{true};
    uint64_t last_picup_ms{0};
    bool initial_format_notified{false};
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

static void parseAnnexBAndSend(struct videnc_state *ves, const uint8_t *data, size_t size)
{
    if (!ves || !data || size == 0) return;

    size_t i = 0;
    std::vector<std::pair<const uint8_t *, size_t>> nals;

    while (i < size) {
        if (i + 3 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            i += 3;
        } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            i += 4;
        } else {
            i++;
            continue;
        }

        const uint8_t *nalStart = data + i;
        size_t nextStart = size;
        for (size_t j = i; j + 2 < size; ++j) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < size && data[j + 2] == 0 && data[j + 3] == 1))) {
                nextStart = j;
                break;
            }
        }
        size_t nalLen = nextStart - i;
        if (nalLen > 0) {
            nals.push_back({nalStart, nalLen});
        }
        i = nextStart;
    }

    for (size_t k = 0; k < nals.size(); ++k) {
        bool isLast = (k == nals.size() - 1);
        sendRfc6184Nal(ves, nals[k].first, nals[k].second, isLast);
    }
}

static void android_h264_enc_destructor(void *arg)
{
    auto *ves = static_cast<struct videnc_state *>(arg);
    if (!ves) return;
    if (ves->encoder) {
        AMediaCodec_stop(ves->encoder);
        AMediaCodec_delete(ves->encoder);
        ves->encoder = nullptr;
    }
    ves->~videnc_state();
}

static int android_h264_enc_update(struct videnc_state **vesp,
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
        void *raw = mem_zalloc(sizeof(struct videnc_state), android_h264_enc_destructor);
        if (!raw) return ENOMEM;
        ves = new (raw) struct videnc_state();
        *vesp = ves;
    }

    ves->prm = *prm;
    ves->pkth = pkth;
    ves->vid = vid;
    return 0;
}

static int ensureEncoderSession(struct videnc_state *ves, unsigned width, unsigned height)
{
    if (ves->encoder && ves->width == width && ves->height == height) {
        return 0;
    }

    if (ves->encoder) {
        AMediaCodec_stop(ves->encoder);
        AMediaCodec_delete(ves->encoder);
        ves->encoder = nullptr;
    }

    ves->encoder = AMediaCodec_createEncoderByType("video/avc");
    if (!ves->encoder) {
        LOGE("Failed to create AMediaCodec encoder for video/avc");
        return EIO;
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, (int32_t)width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, (int32_t)height);
    int bitrate = ves->prm.bitrate > 0 ? (int32_t)ves->prm.bitrate : 512000;
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    int fps = ves->prm.fps > 0 ? (int32_t)ves->prm.fps : 30;
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    ves->color_format = 21; // COLOR_FormatYUV420SemiPlanar (NV12)
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, ves->color_format);

    media_status_t status = AMediaCodec_configure(ves->encoder, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);

    if (status != AMEDIA_OK) {
        LOGE("Failed to configure AMediaCodec encoder (status=%d)", status);
        AMediaCodec_delete(ves->encoder);
        ves->encoder = nullptr;
        return EIO;
    }

    status = AMediaCodec_start(ves->encoder);
    if (status != AMEDIA_OK) {
        LOGE("Failed to start AMediaCodec encoder (status=%d)", status);
        AMediaCodec_delete(ves->encoder);
        ves->encoder = nullptr;
        return EIO;
    }

    ves->width = width;
    ves->height = height;
    LOGI("AMediaCodec encoder started (%ux%u, %d bps, %d fps)", width, height, bitrate, fps);
    return 0;
}

static int android_h264_enc_encode(struct videnc_state *ves,
                                   bool update,
                                   const struct vidframe *frame,
                                   uint64_t timestamp)
{
    if (!ves || !frame || !vidframe_isvalid(frame)) return EINVAL;

    unsigned width = frame->size.w;
    unsigned height = frame->size.h;
    if (ensureEncoderSession(ves, width, height) != 0) {
        return EIO;
    }

    ves->current_rtp_ts = timestamp;

    if (update && ves->encoder) {
        typedef media_status_t (*PFN_AMediaCodec_setParameters)(AMediaCodec *, const AMediaFormat *);
        static auto pfn_setParams = reinterpret_cast<PFN_AMediaCodec_setParameters>(
            dlsym(RTLD_DEFAULT, "AMediaCodec_setParameters"));
        if (pfn_setParams) {
            AMediaFormat *params = AMediaFormat_new();
            AMediaFormat_setInt32(params, "request-sync", 0);
            pfn_setParams(ves->encoder, params);
            AMediaFormat_delete(params);
        }
    }

    ssize_t inputIndex = AMediaCodec_dequeueInputBuffer(ves->encoder, 5000);
    if (inputIndex >= 0) {
        size_t bufSize = 0;
        uint8_t *inputBuf = AMediaCodec_getInputBuffer(ves->encoder, (size_t)inputIndex, &bufSize);
        size_t requiredSize = width * height * 3 / 2;
        if (inputBuf && bufSize >= requiredSize) {
            // Copy Y plane
            for (unsigned y = 0; y < height; ++y) {
                memcpy(inputBuf + y * width, frame->data[0] + y * frame->linesize[0], width);
            }
            // Interleave U and V into NV12
            uint8_t *uvDest = inputBuf + width * height;
            unsigned uvH = height / 2;
            unsigned uvW = width / 2;
            for (unsigned y = 0; y < uvH; ++y) {
                const uint8_t *uLine = frame->data[1] + y * frame->linesize[1];
                const uint8_t *vLine = frame->data[2] + y * frame->linesize[2];
                uint8_t *dstLine = uvDest + y * width;
                for (unsigned x = 0; x < uvW; ++x) {
                    dstLine[x * 2]     = uLine[x];
                    dstLine[x * 2 + 1] = vLine[x];
                }
            }
            uint64_t ptsUs = timestamp * 1000 / 90;
            AMediaCodec_queueInputBuffer(ves->encoder, (size_t)inputIndex, 0, requiredSize, (int64_t)ptsUs, 0);
        } else {
            AMediaCodec_queueInputBuffer(ves->encoder, (size_t)inputIndex, 0, 0, 0, 0);
        }
    }

    AMediaCodecBufferInfo info;
    ssize_t outIndex;
    while ((outIndex = AMediaCodec_dequeueOutputBuffer(ves->encoder, &info, 0)) >= 0) {
        size_t outSize = 0;
        uint8_t *outBuf = AMediaCodec_getOutputBuffer(ves->encoder, (size_t)outIndex, &outSize);
        if (outBuf && info.size > 0) {
            parseAnnexBAndSend(ves, outBuf + info.offset, (size_t)info.size);
        }
        AMediaCodec_releaseOutputBuffer(ves->encoder, (size_t)outIndex, false);
    }

    std::vector<EncodedRtpPacket> packets;
    {
        std::lock_guard<std::mutex> lock(ves->pkt_mutex);
        packets.swap(ves->pending_packets);
    }

    if (ves->pkth) {
        for (const auto &pkt : packets) {
            ves->pkth(pkt.marker,
                      pkt.rtp_ts,
                      pkt.hdr.data(),
                      pkt.hdr.size(),
                      pkt.pld.data(),
                      pkt.pld.size(),
                      ves->vid);
        }
    }

    ves->frame_index++;
    return 0;
}

static void android_h264_dec_destructor(void *arg)
{
    auto *vds = static_cast<struct viddec_state *>(arg);
    if (!vds) return;
    if (vds->decoder) {
        AMediaCodec_stop(vds->decoder);
        AMediaCodec_delete(vds->decoder);
        vds->decoder = nullptr;
    }
    vds->~viddec_state();
}

static int android_h264_dec_update(struct viddec_state **vdsp,
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
        void *raw = mem_zalloc(sizeof(struct viddec_state), android_h264_dec_destructor);
        if (!raw) return ENOMEM;
        vds = new (raw) struct viddec_state();
        *vdsp = vds;
    }
    return 0;
}

static int ensureDecoderSession(struct viddec_state *vds)
{
    ANativeWindow *win = sipsdk::AndroidMediaCodecDriver::get_remote_window();

    if (vds->decoder && vds->current_window == win) {
        return 0;
    }

    if (vds->decoder) {
        AMediaCodec_stop(vds->decoder);
        AMediaCodec_delete(vds->decoder);
        vds->decoder = nullptr;
    }

    vds->decoder = AMediaCodec_createDecoderByType("video/avc");
    if (!vds->decoder) {
        LOGE("Failed to create AMediaCodec decoder for video/avc");
        return EIO;
    }

    AMediaFormat *fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, vds->width > 0 ? (int32_t)vds->width : 640);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, vds->height > 0 ? (int32_t)vds->height : 480);

    media_status_t status = AMediaCodec_configure(vds->decoder, fmt, win, nullptr, 0);
    AMediaFormat_delete(fmt);

    if (status != AMEDIA_OK) {
        LOGE("Failed to configure AMediaCodec decoder (status=%d)", status);
        AMediaCodec_delete(vds->decoder);
        vds->decoder = nullptr;
        return EIO;
    }

    status = AMediaCodec_start(vds->decoder);
    if (status != AMEDIA_OK) {
        LOGE("Failed to start AMediaCodec decoder (status=%d)", status);
        AMediaCodec_delete(vds->decoder);
        vds->decoder = nullptr;
        return EIO;
    }

    vds->current_window = win;
    LOGI("AMediaCodec decoder started with window=%p", win);
    return 0;
}

static void appendNalToAccessUnit(struct viddec_state *vds, const uint8_t *buf, size_t len)
{
    if (!vds || !buf || len == 0) return;
    const uint8_t startCode[4] = {0, 0, 0, 1};
    vds->au_buffer.insert(vds->au_buffer.end(), startCode, startCode + 4);
    vds->au_buffer.insert(vds->au_buffer.end(), buf, buf + len);

    uint8_t nalType = buf[0] & 0x1F;
    if (nalType == H264_NALU_IDR_SLICE || nalType == H264_NALU_SPS || nalType == H264_NALU_PPS) {
        vds->au_is_idr = true;
    }
}

static int android_h264_dec_decode(struct viddec_state *vds,
                                   struct vidframe *frame,
                                   struct viddec_packet *pkt)
{
    (void)frame;
    if (!vds || !pkt || !pkt->mb) return EINVAL;

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

    bool marker = pkt->hdr ? pkt->hdr->m : false;
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
            LOGI("Requesting RTCP PLI keyframe from remote peer (packet loss detected)");
            return EPROTO;
        }
        return 0;
    }

    if (ensureDecoderSession(vds) == 0 && vds->decoder) {
        ssize_t inIdx = AMediaCodec_dequeueInputBuffer(vds->decoder, 2000);
        if (inIdx >= 0) {
            size_t inBufSize = 0;
            uint8_t *inBuf = AMediaCodec_getInputBuffer(vds->decoder, (size_t)inIdx, &inBufSize);
            if (inBuf && inBufSize >= vds->au_buffer.size()) {
                memcpy(inBuf, vds->au_buffer.data(), vds->au_buffer.size());
                int64_t ptsUs = pkt->hdr ? (int64_t)(pkt->hdr->ts * 1000 / 90) : 0;
                AMediaCodec_queueInputBuffer(vds->decoder, (size_t)inIdx, 0, vds->au_buffer.size(), ptsUs, 0);
            } else {
                AMediaCodec_queueInputBuffer(vds->decoder, (size_t)inIdx, 0, 0, 0, 0);
            }
        }

        while (true) {
            AMediaCodecBufferInfo info;
            ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(vds->decoder, &info, 0);
            if (outIdx >= 0) {
                bool doRender = (vds->current_window != nullptr);
                AMediaCodec_releaseOutputBuffer(vds->decoder, (size_t)outIdx, doRender);
                if (!vds->initial_format_notified) {
                    vds->initial_format_notified = true;
                    int notifyW = vds->width > 0 ? static_cast<int>(vds->width) : 640;
                    int notifyH = vds->height > 0 ? static_cast<int>(vds->height) : 480;
                    auto *mgr = sip_core::MediaDriverManager::active_manager();
                    if (mgr) {
                        mgr->handle_stream_resized(notifyW, notifyH);
                    }
                }
            } else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *outFmt = AMediaCodec_getOutputFormat(vds->decoder);
                if (outFmt) {
                    int32_t w = 0, h = 0;
                    AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_WIDTH, &w);
                    AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
                    if (w > 0 && h > 0) {
                        vds->width = (unsigned)w;
                        vds->height = (unsigned)h;
                        vds->initial_format_notified = true;
                        auto *mgr = sip_core::MediaDriverManager::active_manager();
                        if (mgr) {
                            mgr->handle_stream_resized(w, h);
                        }
                    }
                    AMediaFormat_delete(outFmt);
                }
            } else {
                break;
            }
        }
    }

    vds->au_buffer.clear();
    vds->au_is_idr = false;
    return 0;
}

static struct vidcodec gH264MediaCodecMode1 = {
    LE_INIT,
    "96",
    "H264",
    "packetization-mode=1",
    "packetization-mode=1;profile-level-id=42e01f",
    android_h264_enc_update,
    android_h264_enc_encode,
    android_h264_dec_update,
    android_h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};

static struct vidcodec gH264MediaCodecMode0 = {
    LE_INIT,
    "97",
    "H264",
    "packetization-mode=0",
    "packetization-mode=0;profile-level-id=42e01f",
    android_h264_enc_update,
    android_h264_enc_encode,
    android_h264_dec_update,
    android_h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};

static int c_register_codec(void *user_data)
{
    (void)user_data;
    return sipsdk::AndroidMediaCodecDriver::register_codec();
}

static void c_unregister_codec(void *user_data)
{
    (void)user_data;
    sipsdk::AndroidMediaCodecDriver::unregister_codec();
}

namespace sipsdk {

int AndroidMediaCodecDriver::register_codec()
{
    vidcodec_register(baresip_vidcodecl(), &gH264MediaCodecMode1);
    vidcodec_register(baresip_vidcodecl(), &gH264MediaCodecMode0);
    LOGI("Registered Android MediaCodec H.264 vidcodec (modes 1 and 0)");
    return 0;
}

void AndroidMediaCodecDriver::unregister_codec()
{
    vidcodec_unregister(&gH264MediaCodecMode0);
    vidcodec_unregister(&gH264MediaCodecMode1);
    LOGI("Unregistered Android MediaCodec H.264 vidcodec");
}

void AndroidMediaCodecDriver::set_remote_window(ANativeWindow *window)
{
    std::lock_guard<std::mutex> lock(g_window_mutex);
    if (g_remote_window && g_remote_window != window) {
        ANativeWindow_release(g_remote_window);
    }
    g_remote_window = window;
    if (g_remote_window) {
        ANativeWindow_acquire(g_remote_window);
    }
}

ANativeWindow *AndroidMediaCodecDriver::get_remote_window()
{
    std::lock_guard<std::mutex> lock(g_window_mutex);
    return g_remote_window;
}

sip_video_codec_driver_t AndroidMediaCodecDriver::get_c_driver()
{
    sip_video_codec_driver_t d{};
    d.user_data = nullptr;
    d.register_codec = c_register_codec;
    d.unregister_codec = c_unregister_codec;
    return d;
}

} // namespace sipsdk
