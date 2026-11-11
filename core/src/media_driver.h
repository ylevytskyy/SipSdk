#ifndef SIP_CORE_SRC_MEDIA_DRIVER_H
#define SIP_CORE_SRC_MEDIA_DRIVER_H

#include "../include/media_driver.h"
#include "sip_abi.h"

#include <atomic>
#include <memory>
#include <mutex>

struct vidsrc;
struct vidisp;
struct vidframe;

namespace sip_core {

class Engine;

class CApiVideoCaptureDriver : public IVideoCaptureDriver {
public:
    explicit CApiVideoCaptureDriver(const sip_video_capture_driver_t &driver);
    ~CApiVideoCaptureDriver() override;

    int start(int width, int height, int fps) override;
    void stop() override;
    void set_frame_handler(FrameHandler handler) override;
    void switch_camera() override;
    void set_camera(int position) override;
    void set_enabled(bool enabled) override;
    bool is_enabled() const override;

private:
    static void c_frame_callback(const sip_video_frame_t *frame, void *arg);

    sip_video_capture_driver_t m_driver{};

    // Guards m_cpp_handler. c_frame_callback holds it while the handler runs,
    // so set_frame_handler() waits for an in-flight frame before it returns.
    // Not recursive: a frame handler must not call set_frame_handler() on the
    // same adapter.
    std::mutex m_handler_mutex;
    FrameHandler m_cpp_handler;
};

class CApiVideoRendererDriver : public IVideoRendererDriver {
public:
    explicit CApiVideoRendererDriver(const sip_video_renderer_driver_t &driver);
    ~CApiVideoRendererDriver() override;

    void render_frame(const VideoFrame &frame) override;
    void on_stream_resized(int width, int height) override;
    void clear() override;

private:
    sip_video_renderer_driver_t m_driver{};
};

class CApiVideoCodecDriver : public IVideoCodecDriver {
public:
    explicit CApiVideoCodecDriver(const sip_video_codec_driver_t &driver);
    ~CApiVideoCodecDriver() override;

    int register_codec() override;
    void unregister_codec() override;

private:
    sip_video_codec_driver_t m_driver{};
};

class MediaDriverManager {
public:
    explicit MediaDriverManager(Engine *engine);
    ~MediaDriverManager();

    MediaDriverManager(const MediaDriverManager &) = delete;
    MediaDriverManager &operator=(const MediaDriverManager &) = delete;

    void set_capture_driver(std::shared_ptr<IVideoCaptureDriver> driver);
    void set_remote_renderer(std::shared_ptr<IVideoRendererDriver> renderer);
    void set_local_renderer(std::shared_ptr<IVideoRendererDriver> renderer);
    void set_codec_driver(std::shared_ptr<IVideoCodecDriver> driver);

    std::shared_ptr<IVideoCaptureDriver> capture_driver();
    std::shared_ptr<IVideoRendererDriver> remote_renderer();
    std::shared_ptr<IVideoRendererDriver> local_renderer();
    std::shared_ptr<IVideoCodecDriver> codec_driver();

    int register_baresip_plugins();
    void unregister_baresip_plugins();

    void handle_display_frame(const struct vidframe *frame, uint64_t timestamp);
    void handle_stream_resized(int width, int height);
    void handle_display_hide();

    int switch_camera();
    int set_camera(int position);
    int set_local_video_enabled(bool enabled);
    bool is_local_video_enabled() const;
    int start_local_preview();
    int stop_local_preview();
    bool is_local_preview_active() const;

    void on_vidsrc_allocated();
    void on_vidsrc_destroyed();

    static MediaDriverManager *active_manager();

private:
    Engine *m_engine{nullptr};
    mutable std::mutex m_mutex;

    std::shared_ptr<IVideoCaptureDriver> m_user_capture_driver;
    std::shared_ptr<IVideoRendererDriver> m_remote_renderer;
    std::shared_ptr<IVideoRendererDriver> m_local_renderer;
    std::shared_ptr<IVideoCodecDriver> m_codec_driver;

    std::shared_ptr<SyntheticVideoCaptureDriver> m_default_synthetic_driver;
    std::shared_ptr<IVideoCodecDriver> m_default_codec_driver;

    struct vidsrc *m_vidsrc{nullptr};
    struct vidisp *m_vidisp{nullptr};
    bool m_plugins_registered{false};

    std::atomic<bool> m_local_preview_active{false};
    std::atomic<int> m_camera_position{0};
    std::atomic<bool> m_local_video_enabled{true};
    std::atomic<int> m_active_vidsrc_count{0};
};

} // namespace sip_core

#endif // SIP_CORE_SRC_MEDIA_DRIVER_H
