// Unit tests for the macOS sample camera driver (SipMacCameraDriver) and its
// C ABI glue (camera_driver_*). Both are file-private in SipMacClient.mm, so
// this test includes that source file directly.
// Build and run: bash scripts/verify-macos-camera-driver.sh

#include "SipMacClient.mm"

#include <chrono>
#include <cstdio>
#include <thread>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

namespace {

// Records one slow frame handler call. Only the first frame is slow.
struct InFlightProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
};

void slow_frame_handler(const sip_video_frame_t *frame, void *arg)
{
    (void)frame;
    auto *probe = static_cast<InFlightProbe *>(arg);
    if (probe->entered.exchange(true)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    probe->finished.store(true);
}

bool wait_until(const std::atomic<bool> &flag, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Given a frame handler call that is still running on the capture queue,
// when the core stops the camera,
// then camera_driver_stop returns only after that call has finished.
void test_stop_waits_for_in_flight_frame()
{
    @autoreleasepool {
        SipMacCameraDriver *driver = [[SipMacCameraDriver alloc] init];
        [driver setSyntheticMode:YES];
        void *user_data = (__bridge void *)driver;

        InFlightProbe probe;
        camera_driver_set_frame_handler(user_data, slow_frame_handler, &probe);
        TEST_ASSERT(camera_driver_start(user_data, 64, 48, 30) == 0, "synthetic camera must start");
        TEST_ASSERT(wait_until(probe.entered, std::chrono::milliseconds(2000)),
                    "a frame must reach the frame handler");

        camera_driver_stop(user_data);
        bool finished_before_return = probe.finished.load();

        TEST_ASSERT(finished_before_return,
                    "camera_driver_stop returned while a frame handler call was still running");
        camera_driver_set_frame_handler(user_data, nullptr, nullptr);
        std::printf("PASS: test_stop_waits_for_in_flight_frame\n");
    }
}

// Given a frame handler call that is still running on the capture queue,
// when the core clears the frame handler,
// then camera_driver_set_frame_handler returns only after that call has finished.
void test_clearing_handler_waits_for_in_flight_frame()
{
    @autoreleasepool {
        SipMacCameraDriver *driver = [[SipMacCameraDriver alloc] init];
        [driver setSyntheticMode:YES];
        void *user_data = (__bridge void *)driver;

        InFlightProbe probe;
        camera_driver_set_frame_handler(user_data, slow_frame_handler, &probe);
        TEST_ASSERT(camera_driver_start(user_data, 64, 48, 30) == 0, "synthetic camera must start");
        TEST_ASSERT(wait_until(probe.entered, std::chrono::milliseconds(2000)),
                    "a frame must reach the frame handler");

        camera_driver_set_frame_handler(user_data, nullptr, nullptr);
        bool finished_before_return = probe.finished.load();

        TEST_ASSERT(finished_before_return,
                    "camera_driver_set_frame_handler returned while a frame handler call was still running");
        camera_driver_stop(user_data);
        std::printf("PASS: test_clearing_handler_waits_for_in_flight_frame\n");
    }
}

} // namespace

int main()
{
    test_stop_waits_for_in_flight_frame();
    test_clearing_handler_waits_for_in_flight_frame();
    std::printf("ALL_MACOS_CAMERA_DRIVER_TESTS_PASSED\n");
    return 0;
}
