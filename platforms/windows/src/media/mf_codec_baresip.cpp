#include "mf_codec_baresip.h"

#if (defined(_WIN32) || defined(__CYGWIN__)) && defined(SIP_CORE_BUILDING_DLL)

extern "C" {
#include <re.h>
#include <rem.h>
#include <baresip.h>
}

static int mf_h264_enc_update(struct videnc_state **vesp, const struct vidcodec *vc, struct videnc_param *prm, const char *fmtp, videnc_packet_h *pkth, const struct video *vid)
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

void sip_windows_register_h264_codecs()
{
    if (baresip_vidcodecl() != nullptr) {
        vidcodec_register(baresip_vidcodecl(), &g_mf_h264_mode1);
        vidcodec_register(baresip_vidcodecl(), &g_mf_h264_mode0);
    }
}

void sip_windows_unregister_h264_codecs()
{
    vidcodec_unregister(&g_mf_h264_mode0);
    vidcodec_unregister(&g_mf_h264_mode1);
}

#else

void sip_windows_register_h264_codecs() {}
void sip_windows_unregister_h264_codecs() {}

#endif
