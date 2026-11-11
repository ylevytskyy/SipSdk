#include <sip/desktop.hpp>
#include <sip/video_driver.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

int main() {
    std::printf("=================================================================\n");
    std::printf("Running Desktop Video Capture Driver Tests\n");
    std::printf("=================================================================\n");

    // Test 1: Synthetic Video Capture Driver Frame Generation
    std::printf("--- Test 1: Synthetic Video Capture Driver ---\n");
    auto synthetic_driver = std::make_shared<sip_core::SyntheticVideoCaptureDriver>();
    TEST_ASSERT(synthetic_driver != nullptr, "Failed to instantiate SyntheticVideoCaptureDriver");
    TEST_ASSERT(synthetic_driver->is_enabled(), "Driver should be enabled by default");

    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::atomic<int> frame_count{0};
    int last_w = 0;
    int last_h = 0;

    synthetic_driver->set_frame_handler([&](const sip_core::VideoFrame &vf) {
        last_w = vf.width;
        last_h = vf.height;
        frame_count++;
        frame_cv.notify_all();
    });

    int rc = synthetic_driver->start(640, 480, 30);
    TEST_ASSERT(rc == 0, "synthetic_driver->start() failed");

    {
        std::unique_lock<std::mutex> lock(frame_mutex);
        frame_cv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return frame_count.load() >= 3;
        });
    }

    TEST_ASSERT(frame_count.load() >= 3, "Did not receive at least 3 synthetic video frames");
    TEST_ASSERT(last_w == 640 && last_h == 480, "Unexpected frame dimensions");
    synthetic_driver->stop();
    std::printf("PASS: Synthetic frame capture verified (%d frames, %dx%d)\n", frame_count.load(), last_w, last_h);

    // Test 2: V4L2 Video Capture Driver Controls
    std::printf("--- Test 2: V4L2 Driver Controls & Graceful Fallback ---\n");
    auto v4l2_driver = std::make_shared<sip::V4L2VideoCaptureDriver>("/dev/video0");
    TEST_ASSERT(v4l2_driver != nullptr, "Failed to instantiate V4L2VideoCaptureDriver");
    TEST_ASSERT(v4l2_driver->is_enabled(), "V4L2 driver should be enabled by default");
    v4l2_driver->set_enabled(false);
    TEST_ASSERT(!v4l2_driver->is_enabled(), "set_enabled(false) failed");
    v4l2_driver->set_enabled(true);

    // When /dev/video0 does not exist (e.g. macOS host or container without physical camera),
    // start() must return an error code gracefully without crashing.
    rc = v4l2_driver->start(640, 480, 30);
    std::printf("  v4l2_driver->start(/dev/video0) returned: %d (expected on non-v4l2 host)\n", rc);
    v4l2_driver->stop();
    std::printf("PASS: V4L2 driver controls and error handling verified\n");

#if defined(__APPLE__)
    // Test 3: AVFoundation Driver Controls
    std::printf("--- Test 3: AVFoundation Driver Controls ---\n");
    auto avf_driver = std::make_shared<sip::AVFoundationVideoCaptureDriver>();
    TEST_ASSERT(avf_driver != nullptr, "Failed to instantiate AVFoundationVideoCaptureDriver");
    TEST_ASSERT(avf_driver->is_enabled(), "AVFoundation driver should be enabled by default");
    avf_driver->switch_camera();
    avf_driver->set_camera(0);
    avf_driver->stop();
    std::printf("PASS: AVFoundation driver controls verified\n");
#endif

    // Test 4: Default Capture Driver Factory
    std::printf("--- Test 4: Default Capture Driver Factory ---\n");
    auto default_driver = sip::create_default_camera_capture_driver();
    TEST_ASSERT(default_driver != nullptr, "create_default_camera_capture_driver() returned null");
    TEST_ASSERT(default_driver->is_enabled(), "Default capture driver should be enabled");
    std::printf("PASS: Default capture driver factory verified\n");

    // Test 5: Client Video Driver Wiring
    std::printf("--- Test 5: Client Video Driver Integration ---\n");
    class SpyCaptureDriver : public sip_core::IVideoCaptureDriver {
    public:
        sip_core::SyntheticVideoCaptureDriver inner;
        std::atomic<int> start_count{0};
        std::atomic<int> stop_count{0};
        std::atomic<int> frame_count{0};

        int start(int width, int height, int fps) override {
            start_count++;
            return inner.start(width, height, fps);
        }
        void stop() override {
            stop_count++;
            inner.stop();
        }
        void set_frame_handler(sip_core::FrameHandler handler) override {
            inner.set_frame_handler([this, handler](const sip_core::VideoFrame &vf) {
                frame_count++;
                if (handler) handler(vf);
            });
        }
        void switch_camera() override { inner.switch_camera(); }
        void set_camera(int position) override { inner.set_camera(position); }
        void set_enabled(bool enabled) override { inner.set_enabled(enabled); }
        bool is_enabled() const override { return inner.is_enabled(); }
    };

    auto spy_driver = std::make_shared<SpyCaptureDriver>();
    auto client = std::make_shared<sip::Client>("1001", "127.0.0.1");
    client->set_video_capture_driver(spy_driver);
    rc = client->start();
    TEST_ASSERT(rc == 0, "client->start() failed");

    rc = client->start_local_preview();
    TEST_ASSERT(rc == 0, "client->start_local_preview() failed");
    TEST_ASSERT(client->is_local_preview_active(), "Local preview should be active");
    TEST_ASSERT(spy_driver->start_count.load() == 1, "Custom capture driver start() was not invoked by sip_core");

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    TEST_ASSERT(spy_driver->frame_count.load() >= 1, "Custom capture driver did not deliver frames to sip_core");

    rc = client->stop_local_preview();
    TEST_ASSERT(rc == 0, "client->stop_local_preview() failed");
    TEST_ASSERT(!client->is_local_preview_active(), "Local preview should not be active after stop");
    TEST_ASSERT(spy_driver->stop_count.load() == 1, "Custom capture driver stop() was not invoked by sip_core");

    client->stop();
    std::printf("PASS: Client video driver wiring verified\n");

    std::printf("=================================================================\n");
    std::printf("ALL VIDEO CAPTURE DRIVER TESTS PASSED\n");
    std::printf("=================================================================\n");
    return 0;
}
