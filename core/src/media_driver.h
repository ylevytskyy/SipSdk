#ifndef SIP_CORE_SRC_MEDIA_DRIVER_H
#define SIP_CORE_SRC_MEDIA_DRIVER_H

#include "../include/media_driver.h"
#include "call_recorder.h"
#include "conference_mixer.h"
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

class CApiMediaRecorderDriver : public IMediaRecorderDriver {
public:
    explicit CApiMediaRecorderDriver(const sip_media_recorder_driver_t &driver);
    ~CApiMediaRecorderDriver() override = default;

    int start(const char *path, int sample_rate, int channels,
              int video_width, int video_height) override;
    void write_audio(const int16_t *pcm, size_t sample_count, uint64_t timestamp_ns) override;
    void write_video(const RecordedVideoFrame &frame) override;
    int stop() override;

private:
    sip_media_recorder_driver_t m_driver{};
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
    /** Frames that left the decoder since this manager was made, shown or not. */
    uint64_t frames_decoded() const { return m_frames_decoded.load(std::memory_order_relaxed); }
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

    ConferenceMixer &conference_mixer() { return *m_conference_mixer; }
    const ConferenceMixer &conference_mixer() const { return *m_conference_mixer; }
    std::shared_ptr<ConferenceMixer> conference_mixer_shared() const { return m_conference_mixer; }

    CallRecorder &call_recorder() { return *m_call_recorder; }
    const CallRecorder &call_recorder() const { return *m_call_recorder; }
    std::shared_ptr<CallRecorder> call_recorder_shared() const { return m_call_recorder; }
    void set_media_recorder_driver(std::shared_ptr<IMediaRecorderDriver> driver);

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
    std::atomic<uint64_t> m_frames_decoded{0};
    std::shared_ptr<ConferenceMixer> m_conference_mixer{std::make_shared<ConferenceMixer>()};
    std::shared_ptr<CallRecorder> m_call_recorder{std::make_shared<CallRecorder>()};
};

} // namespace sip_core

#endif // SIP_CORE_SRC_MEDIA_DRIVER_H
