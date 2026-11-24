// Unit tests for CApiVideoCaptureDriver (core/src/media_driver.h): the adapter
// between a platform C capture driver and the core C++ frame handler.

#include "media_driver.h"
#include "sip_abi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

namespace {

using FrameCallback = void (*)(const sip_video_frame_t *frame, void *arg);

// Fake platform capture driver. It only stores the C callback that the
// adapter registers; the test calls it from its own "camera" thread.
struct FakePlatformDriver {
    std::atomic<FrameCallback> callback{nullptr};
    std::atomic<void *> callback_arg{nullptr};
};

void fake_set_frame_handler(void *user_data, FrameCallback handler, void *arg)
{
    auto *fake = static_cast<FakePlatformDriver *>(user_data);
    fake->callback_arg.store(arg);
    fake->callback.store(handler);
}

bool fake_is_enabled(void *user_data)
{
    (void)user_data;
    return true;
}

sip_video_capture_driver_t make_c_driver(FakePlatformDriver *fake)
{
    sip_video_capture_driver_t driver{};
    driver.user_data = fake;
    driver.set_frame_handler = fake_set_frame_handler;
    driver.is_enabled = fake_is_enabled;
    return driver;
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

// Given a frame handler call that is still running on the camera thread,
// when the core clears the frame handler,
// then set_frame_handler(nullptr) returns only after that call has finished.
void test_clearing_handler_waits_for_in_flight_frame()
{
    FakePlatformDriver fake;
    sip_core::CApiVideoCaptureDriver adapter(make_c_driver(&fake));

    std::atomic<bool> handler_entered{false};
    std::atomic<bool> handler_finished{false};

    adapter.set_frame_handler([&](const sip_core::VideoFrame &) {
        handler_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        handler_finished.store(true);
    });

    FrameCallback callback = fake.callback.load();
    void *callback_arg = fake.callback_arg.load();
    TEST_ASSERT(callback != nullptr, "adapter must register a C frame callback");

    std::thread camera([callback, callback_arg]() {
        sip_video_frame_t frame{};
        frame.width = 2;
        frame.height = 2;
        callback(&frame, callback_arg);
    });

    TEST_ASSERT(wait_until(handler_entered, std::chrono::milliseconds(2000)),
                "frame handler must start on the camera thread");

    adapter.set_frame_handler(nullptr);
    bool finished_before_return = handler_finished.load();

    camera.join();

    TEST_ASSERT(finished_before_return,
                "set_frame_handler(nullptr) returned while a frame handler call was still running");
    std::printf("PASS: test_clearing_handler_waits_for_in_flight_frame\n");
}

// Given an adapter with a frame handler registered with the platform driver,
// when the adapter is destroyed,
// then the platform driver no longer holds a callback that points at it.
void test_destroying_adapter_clears_platform_callback()
{
    FakePlatformDriver fake;
    {
        sip_core::CApiVideoCaptureDriver adapter(make_c_driver(&fake));
        adapter.set_frame_handler([](const sip_core::VideoFrame &) {});
        TEST_ASSERT(fake.callback.load() != nullptr, "adapter must register a C frame callback");
    }

    TEST_ASSERT(fake.callback.load() == nullptr,
                "destroyed adapter left its frame callback registered with the platform driver");
    TEST_ASSERT(fake.callback_arg.load() == nullptr,
                "destroyed adapter left itself as the platform callback argument");
    std::printf("PASS: test_destroying_adapter_clears_platform_callback\n");
}

} // namespace

int main()
{
    test_clearing_handler_waits_for_in_flight_frame();
    test_destroying_adapter_clears_platform_callback();
    std::printf("ALL_CAPTURE_DRIVER_ADAPTER_TESTS_PASSED\n");
    return 0;
}
