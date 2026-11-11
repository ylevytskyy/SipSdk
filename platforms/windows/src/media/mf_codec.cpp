#include "sip/windows_media.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#if defined(SIP_CORE_BUILDING_DLL)
extern "C" {
#include <re.h>
#include <rem.h>
#include <baresip.h>
}

static int mf_h264_enc_update(struct videnc_state **vesp, const struct vidcodec *vc, struct videnc_prm *prm, const char *fmtp, videnc_packet_h *pkth, const struct video *vid)
{
    (void)vesp; (void)vc; (void)prm; (void)fmtp; (void)pkth; (void)vid;
    return 0;
}

static int mf_h264_enc_encode(struct videnc_state *ves, bool update, const struct vidframe *frame, uint64_t timestamp)
{
    (void)ves; (void)update; (void)frame; (void)timestamp;
    return 0;
}

static int mf_h264_dec_update(struct viddec_state **vdsp, const struct vidcodec *vc, const char *fmtp, const struct video *vid)
{
    (void)vdsp; (void)vc; (void)fmtp; (void)vid;
    return 0;
}

static int mf_h264_dec_decode(struct viddec_state *vds, struct vidframe *frame, struct viddec_packet *pkt)
{
    (void)vds; (void)frame; (void)pkt;
    return 0;
}

static struct vidcodec g_mf_h264_mode1 = {
    LE_INIT,
    "96",
    "H264",
    "packetization-mode=1",
    "packetization-mode=1;profile-level-id=42e01f",
    mf_h264_enc_update,
    mf_h264_enc_encode,
    mf_h264_dec_update,
    mf_h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};

static struct vidcodec g_mf_h264_mode0 = {
    LE_INIT,
    "97",
    "H264",
    "packetization-mode=0",
    "packetization-mode=0;profile-level-id=42e01f",
    mf_h264_enc_update,
    mf_h264_enc_encode,
    mf_h264_dec_update,
    mf_h264_dec_decode,
    nullptr,
    nullptr,
    nullptr,
};
#endif
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
#if defined(SIP_CORE_BUILDING_DLL)
    if (baresip_vidcodecl() != nullptr) {
        vidcodec_register(baresip_vidcodecl(), &g_mf_h264_mode1);
        vidcodec_register(baresip_vidcodecl(), &g_mf_h264_mode0);
    }
#endif
#endif

    m_registered.store(true);
    return 0;
}

void MfVideoCodecDriver::unregister_codec()
{
    if (!m_registered.exchange(false)) {
        return;
    }
#if (defined(_WIN32) || defined(__CYGWIN__)) && defined(SIP_CORE_BUILDING_DLL)
    vidcodec_unregister(&g_mf_h264_mode0);
    vidcodec_unregister(&g_mf_h264_mode1);
#endif
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
