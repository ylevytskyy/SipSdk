#ifndef SIP_WINDOWS_MF_CODEC_BARESIP_H
#define SIP_WINDOWS_MF_CODEC_BARESIP_H

// Registers the Media Foundation H.264 codecs with Baresip. This is kept apart from
// the namespace sip code because libre declares a global struct sip, which cannot
// share a translation unit with namespace sip.
void sip_windows_register_h264_codecs();
void sip_windows_unregister_h264_codecs();

#endif // SIP_WINDOWS_MF_CODEC_BARESIP_H
