#include "sip/windows_media.h"

#include <algorithm>
#include <cstring>

namespace sip::windows {

// Standard ITU-R BT.601 conversion: YUV to RGB/BGRA
static inline uint8_t clamp_u8(int v)
{
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

static void yuv420p_to_bgra(const uint8_t *y_plane,
                            const uint8_t *u_plane,
                            const uint8_t *v_plane,
                            int y_stride,
                            int u_stride,
                            int v_stride,
                            int width,
                            int height,
                            uint8_t *bgra_out)
{
    for (int y = 0; y < height; ++y) {
        const uint8_t *y_row = y_plane + y * y_stride;
        const uint8_t *u_row = u_plane + (y / 2) * u_stride;
        const uint8_t *v_row = v_plane + (y / 2) * v_stride;
        uint8_t *bgra_row = bgra_out + y * (width * 4);

        for (int x = 0; x < width; ++x) {
            int y_val = y_row[x] - 16;
            int u_val = u_row[x / 2] - 128;
            int v_val = v_row[x / 2] - 128;

            if (y_val < 0) y_val = 0;

            int c = 298 * y_val;
            int d = u_val;
            int e = v_val;

            int r = (c + 409 * e + 128) >> 8;
            int g = (c - 100 * d - 208 * e + 128) >> 8;
            int b = (c + 516 * d + 128) >> 8;

            bgra_row[x * 4 + 0] = clamp_u8(b); // B
            bgra_row[x * 4 + 1] = clamp_u8(g); // G
            bgra_row[x * 4 + 2] = clamp_u8(r); // R
            bgra_row[x * 4 + 3] = 255;          // A
        }
    }
}

MfVideoRendererDriver::MfVideoRendererDriver()
{
}

MfVideoRendererDriver::~MfVideoRendererDriver()
{
    clear();
}

void MfVideoRendererDriver::render_frame(const sip_video_frame_t *frame)
{
    if (!frame || !frame->y || !frame->u || !frame->v || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_width != frame->width || m_height != frame->height) {
        m_width = frame->width;
        m_height = frame->height;
        m_bgra_buffer.resize(m_width * m_height * 4);
    }

    yuv420p_to_bgra(frame->y, frame->u, frame->v,
                    frame->y_stride, frame->u_stride, frame->v_stride,
                    frame->width, frame->height,
                    m_bgra_buffer.data());

    m_frame_count++;
}

void MfVideoRendererDriver::on_stream_resized(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (width > 0 && height > 0) {
        m_width = width;
        m_height = height;
        m_bgra_buffer.resize(m_width * m_height * 4, 0);
    }
}

void MfVideoRendererDriver::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fill(m_bgra_buffer.begin(), m_bgra_buffer.end(), 0);
    m_width = 0;
    m_height = 0;
}

void MfVideoRendererDriver::copy_bgra_pixels(std::vector<uint8_t> &out_pixels, int &out_w, int &out_h)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out_w = m_width;
    out_h = m_height;
    out_pixels = m_bgra_buffer;
}

static void c_renderer_render_frame(void *user_data, const sip_video_frame_t *frame)
{
    auto *renderer = static_cast<MfVideoRendererDriver *>(user_data);
    if (renderer) renderer->render_frame(frame);
}

static void c_renderer_on_stream_resized(void *user_data, int width, int height)
{
    auto *renderer = static_cast<MfVideoRendererDriver *>(user_data);
    if (renderer) renderer->on_stream_resized(width, height);
}

static void c_renderer_clear(void *user_data)
{
    auto *renderer = static_cast<MfVideoRendererDriver *>(user_data);
    if (renderer) renderer->clear();
}

sip_video_renderer_driver_t MfVideoRendererDriver::to_c_driver()
{
    sip_video_renderer_driver_t drv;
    drv.user_data = this;
    drv.render_frame = c_renderer_render_frame;
    drv.on_stream_resized = c_renderer_on_stream_resized;
    drv.clear = c_renderer_clear;
    return drv;
}

} // namespace sip::windows
