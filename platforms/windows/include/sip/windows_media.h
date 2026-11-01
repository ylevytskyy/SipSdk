#ifndef SIP_WINDOWS_MEDIA_H
#define SIP_WINDOWS_MEDIA_H

#include "sip_abi.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sip::windows {

/**
 * Windows Media Foundation Video Capture Driver.
 * Uses IMFSourceReader to capture webcam video into YUV420P frames.
 * Falls back to 30 fps synthetic color bars if no hardware camera is present.
 */
class MfVideoCaptureDriver {
public:
    using FrameHandler = void (*)(const sip_video_frame_t *frame, void *arg);

    MfVideoCaptureDriver();
    ~MfVideoCaptureDriver();

    int start(int width, int height, int fps);
    void stop();
    void set_frame_handler(FrameHandler handler, void *arg);
    void set_enabled(bool enabled);
    bool is_enabled() const;
    void switch_camera();
    void set_camera(int position);

    bool is_using_synthetic() const { return m_using_synthetic.load(); }
    uint64_t get_captured_frame_count() const { return m_frame_count.load(); }

    sip_video_capture_driver_t to_c_driver();

private:
    void capture_thread_worker();
    void generate_synthetic_frame(int width, int height, uint32_t tick);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_using_synthetic{false};
    std::atomic<uint64_t> m_frame_count{0};

    int m_width{640};
    int m_height{480};
    int m_fps{30};

    FrameHandler m_handler{nullptr};
    void *m_handler_arg{nullptr};

    std::mutex m_mutex;
    std::vector<uint8_t> m_y_plane;
    std::vector<uint8_t> m_u_plane;
    std::vector<uint8_t> m_v_plane;
};

/**
 * Windows Media Foundation Hardware H.264 Video Codec Driver.
 * Integrates MFT H.264 encoder and decoder with RFC 6184 Constrained Baseline 42e01f.
 */
class MfVideoCodecDriver {
public:
    MfVideoCodecDriver();
    ~MfVideoCodecDriver();

    int register_codec();
    void unregister_codec();
    bool is_registered() const { return m_registered.load(); }

    sip_video_codec_driver_t to_c_driver();

private:
    std::atomic<bool> m_registered{false};
};

/**
 * Windows Media Foundation Video Renderer Driver.
 * Converts decoded YUV420P frame buffers to BGRA32 for UI presentation.
 */
class MfVideoRendererDriver {
public:
    MfVideoRendererDriver();
    ~MfVideoRendererDriver();

    void render_frame(const sip_video_frame_t *frame);
    void on_stream_resized(int width, int height);
    void clear();

    int get_width() const { return m_width; }
    int get_height() const { return m_height; }
    uint64_t get_rendered_frame_count() const { return m_frame_count.load(); }

    // Thread-safe copy of the latest BGRA32 pixel buffer for UI binding (WPF WriteableBitmap)
    void copy_bgra_pixels(std::vector<uint8_t> &out_pixels, int &out_w, int &out_h);

    sip_video_renderer_driver_t to_c_driver();

private:
    mutable std::mutex m_mutex;
    int m_width{0};
    int m_height{0};
    std::atomic<uint64_t> m_frame_count{0};
    std::vector<uint8_t> m_bgra_buffer;
};

} // namespace sip::windows

#endif // SIP_WINDOWS_MEDIA_H
