// Frames that leave the decoder are counted whether or not a renderer is installed.
#include "../src/media_driver.h"
#include "../src/engine.h"
#include "../src/call_session.h"

#include <re.h>
#include <rem.h>
#include <baresip.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s (%s:%d) - %s\n", #cond, __FILE__, __LINE__, msg); \
            std::exit(1); \
        } \
    } while (0)

namespace {

class RecordingRenderer : public sip_core::IVideoRendererDriver {
public:
    int rendered{0};
    void render_frame(const sip_core::VideoFrame &) override { ++rendered; }
    void on_stream_resized(int, int) override {}
    void clear() override {}
};

struct vidframe *make_frame()
{
    struct vidframe *frame = nullptr;
    struct vidsz size = {64, 48};
    int rc = vidframe_alloc(&frame, VID_FMT_YUV420P, &size);
    TEST_ASSERT(rc == 0 && frame != nullptr, "vidframe_alloc failed");
    return frame;
}

} // namespace

int main()
{
    sip_core::MediaDriverManager manager(nullptr);
    struct vidframe *frame = make_frame();

    // A new manager has decoded nothing.
    TEST_ASSERT(manager.frames_decoded() == 0, "a new manager counts no frames");

    // Without a renderer the frames still count.
    for (int i = 0; i < 5; ++i) {
        manager.handle_display_frame(frame, 1000 + i);
    }
    TEST_ASSERT(manager.frames_decoded() == 5, "five frames shown with no renderer count five");
    std::printf("PASS: frames count with no renderer installed\n");

    // With a renderer they count once each and reach the renderer.
    auto renderer = std::make_shared<RecordingRenderer>();
    manager.set_remote_renderer(renderer);
    for (int i = 0; i < 3; ++i) {
        manager.handle_display_frame(frame, 2000 + i);
    }
    TEST_ASSERT(manager.frames_decoded() == 8, "three more frames with a renderer count eight in all");
    TEST_ASSERT(renderer->rendered == 3, "the renderer still gets every frame");
    std::printf("PASS: frames count once each with a renderer installed\n");

    // A null frame is not a frame.
    manager.handle_display_frame(nullptr, 3000);
    TEST_ASSERT(manager.frames_decoded() == 8, "a null frame is not counted");
    std::printf("PASS: a null frame is not counted\n");

    // Per call: a call counts the frames decoded since it began, keeps them once latched,
    // and the next call does not inherit them. (No baresip call exists here, so the packet
    // counts stay zero; a count that must not fall cannot be driven without a live call.)
    {
        sip_core::Engine engine(nullptr, nullptr, nullptr);
        auto &frames = engine.media_driver_manager();
        for (int i = 0; i < 4; ++i) frames.handle_display_frame(frame, 4000 + i);  // before any call

        auto *first = new sip_call_t(nullptr, &engine, nullptr, "sip:a@x", true, true);
        TEST_ASSERT(first->get_media_stats().video_frames_decoded == 0, "nothing is counted before the latch");
        for (int i = 0; i < 7; ++i) frames.handle_display_frame(frame, 5000 + i);
        first->latch_media_stats();
        TEST_ASSERT(first->get_media_stats().video_frames_decoded == 7,
                    "a call counts only the frames decoded since it began");
        first->latch_media_stats();
        TEST_ASSERT(first->get_media_stats().video_frames_decoded == 7, "a second latch keeps the count");
        std::printf("PASS: a call counts the frames since it began and keeps them\n");

        auto *second = new sip_call_t(nullptr, &engine, nullptr, "sip:b@x", true, true);
        for (int i = 0; i < 3; ++i) frames.handle_display_frame(frame, 6000 + i);
        second->latch_media_stats();
        TEST_ASSERT(second->get_media_stats().video_frames_decoded == 3,
                    "a second call does not inherit the first call's frames");
        std::printf("PASS: a second call does not inherit the first call's frames\n");
        first->release();
        second->release();
    }

    mem_deref(frame);
    std::printf("ALL_DISPLAY_FRAME_TESTS_PASSED\n");
    return 0;
}
