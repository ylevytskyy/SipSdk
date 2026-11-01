#ifndef SIP_CORE_MEDIA_DRIVER_H
#define SIP_CORE_MEDIA_DRIVER_H

#include <cstdint>
#include <functional>

namespace sip_core {

struct VideoFrame {
    int width{0};
    int height{0};
    const uint8_t *y{nullptr};
    const uint8_t *u{nullptr};
    const uint8_t *v{nullptr};
    int y_stride{0};
    int u_stride{0};
    int v_stride{0};
    uint64_t timestamp_ns{0};
};

using FrameHandler = std::function<void(const VideoFrame &frame)>;

class IVideoCaptureDriver {
public:
    virtual ~IVideoCaptureDriver() = default;
    virtual int start(int width, int height, int fps) = 0;
    virtual void stop() = 0;
    virtual void set_frame_handler(FrameHandler handler) = 0;
    virtual void switch_camera() = 0;
    virtual void set_camera(int position) = 0;
    virtual void set_enabled(bool enabled) = 0;
    virtual bool is_enabled() const = 0;
};

class IVideoRendererDriver {
public:
    virtual ~IVideoRendererDriver() = default;
    virtual void render_frame(const VideoFrame &frame) = 0;
    virtual void on_stream_resized(int width, int height) = 0;
    virtual void clear() = 0;
};

class IVideoCodecDriver {
public:
    virtual ~IVideoCodecDriver() = default;
    virtual int register_codec() = 0;
    virtual void unregister_codec() = 0;
};

class SyntheticVideoCaptureDriver : public IVideoCaptureDriver {
public:
    SyntheticVideoCaptureDriver();
    ~SyntheticVideoCaptureDriver() override;

    int start(int width, int height, int fps) override;
    void stop() override;
    void set_frame_handler(FrameHandler handler) override;
    void switch_camera() override;
    void set_camera(int position) override;
    void set_enabled(bool enabled) override;
    bool is_enabled() const override;

private:
    struct Impl;
    Impl *m_impl{nullptr};
};

} // namespace sip_core

#endif // SIP_CORE_MEDIA_DRIVER_H
