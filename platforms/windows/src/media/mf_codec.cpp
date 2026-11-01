#include "sip/windows_media.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#endif

namespace sip::windows {

MfVideoCodecDriver::MfVideoCodecDriver()
{
}

MfVideoCodecDriver::~MfVideoCodecDriver()
{
    unregister_codec();
}

int MfVideoCodecDriver::register_codec()
{
    if (m_registered.load()) {
        return 0;
    }

#if defined(_WIN32) || defined(__CYGWIN__)
    // Verify MFT H.264 encoder availability
    IMFTransform *encoder = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_IMFTransform, reinterpret_cast<void **>(&encoder));
    if (SUCCEEDED(hr) && encoder) {
        encoder->Release();
    }
#endif

    m_registered.store(true);
    return 0;
}

void MfVideoCodecDriver::unregister_codec()
{
    m_registered.store(false);
}

static int c_codec_register(void *user_data)
{
    auto *driver = static_cast<MfVideoCodecDriver *>(user_data);
    return driver ? driver->register_codec() : -1;
}

static void c_codec_unregister(void *user_data)
{
    auto *driver = static_cast<MfVideoCodecDriver *>(user_data);
    if (driver) driver->unregister_codec();
}

sip_video_codec_driver_t MfVideoCodecDriver::to_c_driver()
{
    sip_video_codec_driver_t drv;
    drv.user_data = this;
    drv.register_codec = c_codec_register;
    drv.unregister_codec = c_codec_unregister;
    return drv;
}

} // namespace sip::windows
