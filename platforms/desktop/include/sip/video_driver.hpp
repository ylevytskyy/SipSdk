#ifndef SIP_DESKTOP_VIDEO_DRIVER_HPP
#define SIP_DESKTOP_VIDEO_DRIVER_HPP

#include <media_driver.h>
#include <memory>
#include <string>

namespace sip {

#if defined(__APPLE__)
class AVFoundationVideoCaptureDriver : public sip_core::IVideoCaptureDriver {
public:
    AVFoundationVideoCaptureDriver();
    ~AVFoundationVideoCaptureDriver() override;

    int start(int width, int height, int fps) override;
    void stop() override;
    void set_frame_handler(sip_core::FrameHandler handler) override;
    void switch_camera() override;
    void set_camera(int position) override;
    void set_enabled(bool enabled) override;
    bool is_enabled() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif

class V4L2VideoCaptureDriver : public sip_core::IVideoCaptureDriver {
public:
    explicit V4L2VideoCaptureDriver(std::string device_path = "/dev/video0");
    ~V4L2VideoCaptureDriver() override;

    int start(int width, int height, int fps) override;
    void stop() override;
    void set_frame_handler(sip_core::FrameHandler handler) override;
    void switch_camera() override;
    void set_camera(int position) override;
    void set_enabled(bool enabled) override;
    bool is_enabled() const override;

    const std::string &device_path() const { return m_device_path; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_device_path;
};

std::shared_ptr<sip_core::IVideoCaptureDriver> create_default_camera_capture_driver();

} // namespace sip

#endif // SIP_DESKTOP_VIDEO_DRIVER_HPP
