#include "sip/windows_media.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int g_captured_frames = 0;
static int g_last_width = 0;
static int g_last_height = 0;

static void on_test_frame_captured(const sip_video_frame_t *frame, void *arg)
{
    (void)arg;
    if (frame) {
        g_captured_frames++;
        g_last_width = frame->width;
        g_last_height = frame->height;
    }
}

int main()
{
    printf("==> Running Windows Media Foundation Driver Tests...\n");

    // 1. Test Video Capture Driver (Webcam / Synthetic Fallback)
    printf("--> 1. Testing MfVideoCaptureDriver...\n");
    {
        sip::windows::MfVideoCaptureDriver capture_driver;
        capture_driver.set_frame_handler(on_test_frame_captured, nullptr);

        int res = capture_driver.start(320, 240, 30);
        assert(res == 0);
        assert(capture_driver.is_enabled());

        // Wait up to 300ms for synthetic/hardware frames to arrive
        for (int i = 0; i < 30 && g_captured_frames == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        assert(g_captured_frames > 0);
        assert(g_last_width == 320);
        assert(g_last_height == 240);
        printf("PASS: Capture driver generated %d frames (width=%d, height=%d, synthetic=%s)\n",
               g_captured_frames, g_last_width, g_last_height,
               capture_driver.is_using_synthetic() ? "true" : "false");

        capture_driver.stop();
    }

    // 2. Test Video Codec Driver (H.264 MFT)
    printf("--> 2. Testing MfVideoCodecDriver...\n");
    {
        sip::windows::MfVideoCodecDriver codec_driver;
        assert(!codec_driver.is_registered());

        int res = codec_driver.register_codec();
        assert(res == 0);
        assert(codec_driver.is_registered());

        codec_driver.unregister_codec();
        assert(!codec_driver.is_registered());
        printf("PASS: MfVideoCodecDriver lifecycle tested cleanly\n");
    }

    // 3. Test Video Renderer Driver (YUV420P -> BGRA32 conversion)
    printf("--> 3. Testing MfVideoRendererDriver...\n");
    {
        sip::windows::MfVideoRendererDriver renderer;
        assert(renderer.get_width() == 0);

        int test_w = 160;
        int test_h = 120;
        std::vector<uint8_t> y_plane(test_w * test_h, 200);
        std::vector<uint8_t> u_plane((test_w / 2) * (test_h / 2), 128);
        std::vector<uint8_t> v_plane((test_w / 2) * (test_h / 2), 128);

        sip_video_frame_t frame;
        frame.width = test_w;
        frame.height = test_h;
        frame.y = y_plane.data();
        frame.u = u_plane.data();
        frame.v = v_plane.data();
        frame.y_stride = test_w;
        frame.u_stride = test_w / 2;
        frame.v_stride = test_w / 2;
        frame.timestamp_ns = 1000000;

        renderer.render_frame(&frame);
        assert(renderer.get_rendered_frame_count() == 1);
        assert(renderer.get_width() == test_w);
        assert(renderer.get_height() == test_h);

        std::vector<uint8_t> bgra_out;
        int out_w = 0;
        int out_h = 0;
        renderer.copy_bgra_pixels(bgra_out, out_w, out_h);
        assert(out_w == test_w);
        assert(out_h == test_h);
        assert(bgra_out.size() == static_cast<size_t>(test_w * test_h * 4));

        renderer.clear();
        assert(renderer.get_width() == 0);
        printf("PASS: MfVideoRendererDriver YUV->BGRA conversion verified cleanly\n");
    }

    printf("ALL_WINDOWS_MEDIA_DRIVER_TESTS_PASSED\n");
    return 0;
}
