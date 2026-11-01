#ifndef MEDIA_CODEC_DRIVER_H
#define MEDIA_CODEC_DRIVER_H

#include "sip_abi.h"
#include <android/native_window.h>
#include <memory>

namespace sipsdk {

class AndroidMediaCodecDriver {
public:
    static int register_codec();
    static void unregister_codec();

    static void set_remote_window(ANativeWindow *window);
    static ANativeWindow *get_remote_window();

    static sip_video_codec_driver_t get_c_driver();
};

} // namespace sipsdk

#endif // MEDIA_CODEC_DRIVER_H
