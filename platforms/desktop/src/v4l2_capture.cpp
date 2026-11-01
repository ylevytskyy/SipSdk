#include <sip/video_driver.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sip {

struct V4L2Buffer {
    void *start{nullptr};
    size_t length{0};
};

struct V4L2VideoCaptureDriver::Impl {
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{true};
    std::atomic<int> camera_position{0};
    int fd{-1};
    int width{640};
    int height{480};
    int fps{30};
    uint32_t pixel_format{0};
    std::vector<V4L2Buffer> buffers;
    sip_core::FrameHandler frame_handler;
    std::mutex mutex;
    std::thread thread;

    ~Impl() {
        stop_capture();
    }

    int start_capture(const std::string &dev_path, int req_w, int req_h, int req_fps) {
#if defined(__linux__)
        std::lock_guard<std::mutex> lock(mutex);
        if (running.load()) {
            return 0;
        }

        fd = open(dev_path.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) {
            return -1;
        }

        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            close(fd);
            fd = -1;
            return -1;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            close(fd);
            fd = -1;
            return -1;
        }

        struct v4l2_format fmt;
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = req_w;
        fmt.fmt.pix.height = req_h;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
                close(fd);
                fd = -1;
                return -1;
            }
        }

        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
        fps = req_fps;
        pixel_format = fmt.fmt.pix.pixelformat;

        struct v4l2_requestbuffers req;
        std::memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            close(fd);
            fd = -1;
            return -1;
        }

        buffers.resize(req.count);
        for (size_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                cleanup_buffers();
                close(fd);
                fd = -1;
                return -1;
            }

            buffers[i].length = buf.length;
            buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
            if (buffers[i].start == MAP_FAILED) {
                cleanup_buffers();
                close(fd);
                fd = -1;
                return -1;
            }

            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                cleanup_buffers();
                close(fd);
                fd = -1;
                return -1;
            }
        }

        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            cleanup_buffers();
            close(fd);
            fd = -1;
            return -1;
        }

        running = true;
        thread = std::thread([this]() { this->loop(); });
        return 0;
#else
        (void)dev_path;
        (void)req_w;
        (void)req_h;
        (void)req_fps;
        return -1;
#endif
    }

    void stop_capture() {
#if defined(__linux__)
        running = false;
        if (thread.joinable()) {
            thread.join();
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (fd >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd, VIDIOC_STREAMOFF, &type);
            cleanup_buffers();
            close(fd);
            fd = -1;
        }
#endif
    }

    void cleanup_buffers() {
#if defined(__linux__)
        for (auto &b : buffers) {
            if (b.start && b.start != MAP_FAILED) {
                munmap(b.start, b.length);
                b.start = nullptr;
            }
        }
        buffers.clear();
#endif
    }

    void loop() {
#if defined(__linux__)
        std::vector<uint8_t> yuv420_buffer;
        yuv420_buffer.resize(width * height * 3 / 2);

        while (running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int r = select(fd + 1, &fds, NULL, NULL, &tv);
            if (r <= 0) continue;

            struct v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                continue;
            }

            if (enabled.load()) {
                const uint8_t *raw_data = static_cast<const uint8_t *>(buffers[buf.index].start);
                sip_core::VideoFrame vf;
                vf.width = width;
                vf.height = height;

                if (pixel_format == V4L2_PIX_FMT_YUV420) {
                    vf.y = raw_data;
                    vf.u = raw_data + (width * height);
                    vf.v = vf.u + (width * height / 4);
                    vf.y_stride = width;
                    vf.u_stride = width / 2;
                    vf.v_stride = width / 2;
                } else if (pixel_format == V4L2_PIX_FMT_YUYV) {
                    // Convert YUYV (YUY2) to YUV420P
                    uint8_t *y_out = yuv420_buffer.data();
                    uint8_t *u_out = y_out + (width * height);
                    uint8_t *v_out = u_out + (width * height / 4);

                    for (int y = 0; y < height; ++y) {
                        const uint8_t *src_line = raw_data + (y * width * 2);
                        uint8_t *dst_y_line = y_out + (y * width);
                        for (int x = 0; x < width; x += 2) {
                            dst_y_line[x] = src_line[x * 2];
                            dst_y_line[x + 1] = src_line[x * 2 + 2];
                            if ((y % 2) == 0) {
                                int uv_idx = (y / 2) * (width / 2) + (x / 2);
                                u_out[uv_idx] = src_line[x * 2 + 1];
                                v_out[uv_idx] = src_line[x * 2 + 3];
                            }
                        }
                    }

                    vf.y = y_out;
                    vf.u = u_out;
                    vf.v = v_out;
                    vf.y_stride = width;
                    vf.u_stride = width / 2;
                    vf.v_stride = width / 2;
                }

                auto now = std::chrono::steady_clock::now().time_since_epoch();
                vf.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

                sip_core::FrameHandler handler_copy;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    handler_copy = frame_handler;
                }
                if (handler_copy) {
                    handler_copy(vf);
                }
            }

            ioctl(fd, VIDIOC_QBUF, &buf);
        }
#endif
    }
};

V4L2VideoCaptureDriver::V4L2VideoCaptureDriver(std::string device_path)
    : m_impl(std::make_unique<Impl>())
    , m_device_path(std::move(device_path))
{
}

V4L2VideoCaptureDriver::~V4L2VideoCaptureDriver() = default;

int V4L2VideoCaptureDriver::start(int width, int height, int fps)
{
    return m_impl->start_capture(m_device_path, width, height, fps);
}

void V4L2VideoCaptureDriver::stop()
{
    m_impl->stop_capture();
}

void V4L2VideoCaptureDriver::set_frame_handler(sip_core::FrameHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->frame_handler = std::move(handler);
}

void V4L2VideoCaptureDriver::switch_camera()
{
    m_impl->camera_position = (m_impl->camera_position == 0) ? 1 : 0;
}

void V4L2VideoCaptureDriver::set_camera(int position)
{
    m_impl->camera_position = position;
}

void V4L2VideoCaptureDriver::set_enabled(bool enabled)
{
    m_impl->enabled = enabled;
}

bool V4L2VideoCaptureDriver::is_enabled() const
{
    return m_impl->enabled.load();
}

#if !defined(__APPLE__)
std::shared_ptr<sip_core::IVideoCaptureDriver> create_default_camera_capture_driver()
{
    return std::make_shared<V4L2VideoCaptureDriver>();
}
#endif

} // namespace sip
