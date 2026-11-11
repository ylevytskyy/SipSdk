#include "sip/windows_media.h"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace sip::windows {

static void fill_synthetic_pattern(uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane, int width, int height, uint32_t tick)
{
    static const uint8_t kBarY[8] = {235, 210, 170, 145, 106,  81,  41,  24};
    static const uint8_t kBarU[8] = {128,  16, 166,  54, 202,  90, 240, 128};
    static const uint8_t kBarV[8] = {128, 146,  16,  34, 222, 240, 110, 128};

    int bar_width = width / 8;
    if (bar_width < 1) bar_width = 1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int bar_idx = (x / bar_width) & 7;
            y_plane[y * width + x] = kBarY[bar_idx];
        }
    }

    int uv_w = width / 2;
    int uv_h = height / 2;
    for (int y = 0; y < uv_h; ++y) {
        for (int x = 0; x < uv_w; ++x) {
            int bar_idx = ((x * 2) / bar_width) & 7;
            u_plane[y * uv_w + x] = kBarU[bar_idx];
            v_plane[y * uv_w + x] = kBarV[bar_idx];
        }
    }

    // Moving timestamp box indicator
    int box_w = width / 8;
    int box_h = height / 8;
    if (box_w < 8) box_w = 8;
    if (box_h < 8) box_h = 8;

    int box_x = (tick * 4) % (width - box_w);
    int box_y = (tick * 2) % (height - box_h);

    for (int y = box_y; y < box_y + box_h; ++y) {
        for (int x = box_x; x < box_x + box_w; ++x) {
            y_plane[y * width + x] = 235; // Bright white box
        }
    }
}

MfVideoCaptureDriver::MfVideoCaptureDriver()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    MFStartup(MF_VERSION);
#endif
}

MfVideoCaptureDriver::~MfVideoCaptureDriver()
{
    stop();
#if defined(_WIN32) || defined(__CYGWIN__)
    MFShutdown();
#endif
}

int MfVideoCaptureDriver::start(int width, int height, int fps)
{
    if (m_running.load()) {
        return 0;
    }

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_width = width > 0 ? width : 640;
        m_height = height > 0 ? height : 480;
        m_fps = fps > 0 ? fps : 30;

        m_y_plane.resize(m_width * m_height);
        m_u_plane.resize((m_width / 2) * (m_height / 2));
        m_v_plane.resize((m_width / 2) * (m_height / 2));
    }

    bool found_hw_camera = false;
#if defined(_WIN32) || defined(__CYGWIN__)
    IMFAttributes *attributes = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&attributes, 1))) {
        attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate **devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attributes, &devices, &count)) && count > 0) {
            UINT32 idx = static_cast<UINT32>(m_camera_index.load() >= 0 ? m_camera_index.load() : 0) % count;
            IMFMediaSource *source = nullptr;
            if (SUCCEEDED(devices[idx]->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void **>(&source))) && source) {
                IMFSourceReader *reader = nullptr;
                if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader)) && reader) {
                    IMFMediaType *media_type = nullptr;
                    if (SUCCEEDED(MFCreateMediaType(&media_type)) && media_type) {
                        media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                        media_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                        MFSetAttributeSize(media_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(m_width), static_cast<UINT32>(m_height));
                        MFSetAttributeRatio(media_type, MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
                        reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, media_type);
                        media_type->Release();
                    }
                    m_media_source = source;
                    m_source_reader = reader;
                    found_hw_camera = true;
                } else {
                    source->Shutdown();
                    source->Release();
                }
            }
            for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
            CoTaskMemFree(devices);
        }
        attributes->Release();
    }
#endif

    m_using_synthetic.store(!found_hw_camera);
    m_running.store(true);

    m_worker_thread = std::thread([this]() {
        capture_thread_worker();
    });

    return 0;
}

void MfVideoCaptureDriver::stop()
{
    m_running.store(false);
    if (m_worker_thread.joinable() && m_worker_thread.get_id() != std::this_thread::get_id()) {
        m_worker_thread.join();
    }
#if defined(_WIN32) || defined(__CYGWIN__)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_source_reader) {
        static_cast<IMFSourceReader *>(m_source_reader)->Release();
        m_source_reader = nullptr;
    }
    if (m_media_source) {
        auto *src = static_cast<IMFMediaSource *>(m_media_source);
        src->Shutdown();
        src->Release();
        m_media_source = nullptr;
    }
#endif
}

void MfVideoCaptureDriver::set_frame_handler(FrameHandler handler, void *arg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handler = handler;
    m_handler_arg = arg;
}

void MfVideoCaptureDriver::set_enabled(bool enabled)
{
    m_enabled.store(enabled);
}

bool MfVideoCaptureDriver::is_enabled() const
{
    return m_enabled.load();
}

void MfVideoCaptureDriver::switch_camera()
{
    m_camera_index.store((m_camera_index.load() == 0) ? 1 : 0);
}

void MfVideoCaptureDriver::set_camera(int position)
{
    m_camera_index.store(position);
}

void MfVideoCaptureDriver::capture_thread_worker()
{
    uint32_t tick = 0;
    const auto frame_duration = std::chrono::milliseconds(1000 / m_fps);

    while (m_running.load()) {
        auto start_time = std::chrono::steady_clock::now();

        if (m_enabled.load()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running.load()) {
                break;
            }
            bool got_hw_frame = false;
#if defined(_WIN32) || defined(__CYGWIN__)
            if (m_source_reader && !m_using_synthetic.load()) {
                auto *reader = static_cast<IMFSourceReader *>(m_source_reader);
                DWORD stream_index = 0;
                DWORD flags = 0;
                LONGLONG ll_time_stamp = 0;
                IMFSample *sample = nullptr;
                HRESULT hr = reader->ReadSample(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    0,
                    &stream_index,
                    &flags,
                    &ll_time_stamp,
                    &sample
                );
                if (SUCCEEDED(hr) && sample) {
                    IMFMediaBuffer *buffer = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                        BYTE *raw_data = nullptr;
                        DWORD max_len = 0;
                        DWORD cur_len = 0;
                        if (SUCCEEDED(buffer->Lock(&raw_data, &max_len, &cur_len)) && raw_data) {
                            const size_t y_size = static_cast<size_t>(m_width * m_height);
                            const size_t uv_size = static_cast<size_t>((m_width / 2) * (m_height / 2));
                            if (cur_len >= y_size + uv_size * 2) {
                                std::memcpy(m_y_plane.data(), raw_data, y_size);
                                const uint8_t *uv_src = raw_data + y_size;
                                for (size_t i = 0; i < uv_size; ++i) {
                                    m_u_plane[i] = uv_src[i * 2];
                                    m_v_plane[i] = uv_src[i * 2 + 1];
                                }
                                got_hw_frame = true;
                            }
                            buffer->Unlock();
                        }
                        buffer->Release();
                    }
                    sample->Release();
                }
            }
#endif
            if (!got_hw_frame) {
                fill_synthetic_pattern(m_y_plane.data(), m_u_plane.data(), m_v_plane.data(), m_width, m_height, tick);
            }

            if (m_handler) {
                sip_video_frame_t frame;
                frame.width = m_width;
                frame.height = m_height;
                frame.y = m_y_plane.data();
                frame.u = m_u_plane.data();
                frame.v = m_v_plane.data();
                frame.y_stride = m_width;
                frame.u_stride = m_width / 2;
                frame.v_stride = m_width / 2;
                frame.timestamp_ns = static_cast<uint64_t>(tick) * (1000000000ULL / m_fps);

                m_handler(&frame, m_handler_arg);
                m_frame_count++;
            }
        }

        tick++;
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed < frame_duration && m_running.load()) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }
}

// C ABI Adapter
static int c_driver_start(void *user_data, int width, int height, int fps)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    return driver ? driver->start(width, height, fps) : -1;
}

static void c_driver_stop(void *user_data)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    if (driver) driver->stop();
}

static void c_driver_set_frame_handler(void *user_data, void (*handler)(const sip_video_frame_t *, void *), void *arg)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    if (driver) driver->set_frame_handler(handler, arg);
}

static void c_driver_switch_camera(void *user_data)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    if (driver) driver->switch_camera();
}

static void c_driver_set_camera(void *user_data, int position)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    if (driver) driver->set_camera(position);
}

static void c_driver_set_enabled(void *user_data, bool enabled)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    if (driver) driver->set_enabled(enabled);
}

static bool c_driver_is_enabled(void *user_data)
{
    auto *driver = static_cast<MfVideoCaptureDriver *>(user_data);
    return driver && driver->is_enabled();
}

sip_video_capture_driver_t MfVideoCaptureDriver::to_c_driver()
{
    sip_video_capture_driver_t drv;
    drv.user_data = this;
    drv.start = c_driver_start;
    drv.stop = c_driver_stop;
    drv.set_frame_handler = c_driver_set_frame_handler;
    drv.switch_camera = c_driver_switch_camera;
    drv.set_camera = c_driver_set_camera;
    drv.set_enabled = c_driver_set_enabled;
    drv.is_enabled = c_driver_is_enabled;
    return drv;
}

} // namespace sip::windows
