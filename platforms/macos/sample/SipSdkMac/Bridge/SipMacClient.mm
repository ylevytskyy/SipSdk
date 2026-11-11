#import "SipMacClient.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreAudio/CoreAudio.h>
#import <AppKit/AppKit.h>
#include <sip_abi.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

// Forward declaration of internal camera driver
@class SipMacCameraDriver;

@interface SipMacClient () {
@public
    sip_client_t *_client;
    std::mutex _mutex;
    std::unordered_map<sip_call_t *, SipMacCall *> _activeCalls;

    __weak SipMacVideoView *_remoteVideoView;
    __weak SipMacVideoView *_localPreviewView;

    BOOL _useSyntheticCamera;
    NSString *_selectedCameraId;
    SipMacCameraDriver *_cameraDriver;

    int _port;
    NSString *_transport;

    NSString *_selectedAudioInputDevice;
    NSString *_selectedAudioOutputDevice;
    NSString *_selectedCameraName;
    NSString *_selectedVideoOutputDevice;
}
- (SipMacCall *)getOrCreateCall:(sip_call_t *)handle;
- (void)removeCall:(sip_call_t *)handle;
@end

// ============================================================================
// MARK: - Internal Camera Capture Driver
// ============================================================================

@interface SipMacCameraDriver : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    void (*_frameHandler)(const sip_video_frame_t *frame, void *arg);
    void *_frameHandlerArg;
    std::atomic<bool> _running;
    std::atomic<bool> _enabled;
    std::atomic<bool> _syntheticMode;
    std::recursive_mutex _driverMutex;
    uint32_t _syntheticTick;

    AVCaptureSession *_session;
    dispatch_queue_t _captureQueue;
    dispatch_source_t _syntheticTimer;
    NSString *_selectedDeviceId;
}
- (instancetype)init;
- (int)startWithWidth:(int)width height:(int)height fps:(int)fps;
- (void)stop;
- (void)setSyntheticMode:(BOOL)synthetic;
- (void)setCameraDeviceId:(NSString *)deviceId;
- (NSArray<AVCaptureDevice *> *)availableDevices;
@end

@implementation SipMacCameraDriver

- (instancetype)init
{
    self = [super init];
    if (self) {
        _frameHandler = nullptr;
        _frameHandlerArg = nullptr;
        _running = false;
        _enabled = true;
        _syntheticMode = false;
        _syntheticTick = 0;
        _captureQueue = dispatch_queue_create("com.sipsdk.mac.camera_capture", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)setSyntheticMode:(BOOL)synthetic
{
    std::lock_guard<std::recursive_mutex> lock(_driverMutex);
    _syntheticMode = synthetic;
    if (_running.load()) {
        [self stop];
        [self startWithWidth:640 height:480 fps:30];
    }
}

- (NSArray<AVCaptureDevice *> *)availableDevices
{
    if (@available(macOS 14.0, *)) {
        AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        return discovery.devices;
    } else if (@available(macOS 10.15, *)) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVCaptureDeviceDiscoverySession *discovery = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        return discovery.devices;
#pragma clang diagnostic pop
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        return [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
    }
}

- (void)setCameraDeviceId:(NSString *)deviceId
{
    std::lock_guard<std::recursive_mutex> lock(_driverMutex);
    _selectedDeviceId = [deviceId copy];
    if (_running.load() && !_syntheticMode.load()) {
        if (_syntheticTimer) {
            dispatch_source_cancel(_syntheticTimer);
            _syntheticTimer = nil;
        }
        if (_session) {
            [_session stopRunning];
            _session = nil;
        }
        _running = false;
        [self startWithWidth:640 height:480 fps:30];
    }
}

- (void)startSyntheticTimerWithWidth:(int)width height:(int)height fps:(int)fps
{
    uint64_t intervalNs = (fps > 0) ? (1000000000ULL / (uint64_t)fps) : (1000000000ULL / 30ULL);
    _syntheticTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _captureQueue);
    dispatch_source_set_timer(_syntheticTimer, dispatch_time(DISPATCH_TIME_NOW, intervalNs), intervalNs, 1000000ULL);

    size_t ySize = (size_t)width * (size_t)height;
    size_t uvSize = (size_t)(width / 2) * (size_t)(height / 2);
    auto yPlane = std::make_shared<std::vector<uint8_t>>(ySize);
    auto uPlane = std::make_shared<std::vector<uint8_t>>(uvSize);
    auto vPlane = std::make_shared<std::vector<uint8_t>>(uvSize);

    __weak SipMacCameraDriver *weakSelf = self;
    dispatch_source_set_event_handler(_syntheticTimer, ^{
        SipMacCameraDriver *strongSelf = weakSelf;
        if (!strongSelf || !strongSelf->_running.load() || !strongSelf->_enabled.load() || !strongSelf->_frameHandler) {
            return;
        }

        uint32_t tick = strongSelf->_syntheticTick++;
        int bar_width = width / 8;
        if (bar_width < 1) bar_width = 1;

        static const uint8_t kBarY[8] = {235, 210, 170, 145, 106,  81,  41,  24};
        static const uint8_t kBarU[8] = {128,  16, 166,  54, 202,  90, 240, 128};
        static const uint8_t kBarV[8] = {128, 146,  16,  34, 222, 240, 110, 128};

        uint8_t *y_ptr = yPlane->data();
        uint8_t *u_ptr = uPlane->data();
        uint8_t *v_ptr = vPlane->data();

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int bar_idx = (x / bar_width) & 7;
                y_ptr[y * width + x] = kBarY[bar_idx];
            }
        }

        int uv_w = width / 2;
        int uv_h = height / 2;
        for (int y = 0; y < uv_h; ++y) {
            for (int x = 0; x < uv_w; ++x) {
                int bar_idx = ((x * 2) / bar_width) & 7;
                u_ptr[y * uv_w + x] = kBarU[bar_idx];
                v_ptr[y * uv_w + x] = kBarV[bar_idx];
            }
        }

        // Bouncing box animation
        int box_w = width / 8;
        int box_h = height / 8;
        int max_x = width - box_w;
        int max_y = height - box_h;
        if (max_x > 0 && max_y > 0) {
            int box_x = (int)((tick * 6) % (unsigned)max_x);
            int box_y = (int)((tick * 3) % (unsigned)max_y);
            for (int y = box_y; y < box_y + box_h && y < height; ++y) {
                for (int x = box_x; x < box_x + box_w && x < width; ++x) {
                    y_ptr[y * width + x] = 235;
                    if ((y / 2) < uv_h && (x / 2) < uv_w) {
                        u_ptr[(y / 2) * uv_w + (x / 2)] = 180;
                        v_ptr[(y / 2) * uv_w + (x / 2)] = 80;
                    }
                }
            }
        }

        sip_video_frame_t frame;
        frame.width = width;
        frame.height = height;
        frame.y = y_ptr;
        frame.u = u_ptr;
        frame.v = v_ptr;
        frame.y_stride = width;
        frame.u_stride = uv_w;
        frame.v_stride = uv_w;
        frame.timestamp_ns = (uint64_t)tick * intervalNs;

        strongSelf->_frameHandler(&frame, strongSelf->_frameHandlerArg);
    });

    dispatch_resume(_syntheticTimer);
}

- (int)startWithWidth:(int)width height:(int)height fps:(int)fps
{
    std::lock_guard<std::recursive_mutex> lock(_driverMutex);
    if (_running.load()) return 0;

    _running = true;

    if (_syntheticMode.load()) {
        [self startSyntheticTimerWithWidth:width height:height fps:fps];
        return 0;
    }

    NSArray<AVCaptureDevice *> *devices = [self availableDevices];
    if (devices.count == 0) {
        // Fall back to synthetic test pattern if no physical camera exists
        _syntheticMode = true;
        [self startSyntheticTimerWithWidth:width height:height fps:fps];
        return 0;
    }

    _session = [[AVCaptureSession alloc] init];
    if ([_session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
        _session.sessionPreset = AVCaptureSessionPreset640x480;
    }

    AVCaptureDevice *device = nil;
    if (_selectedDeviceId && _selectedDeviceId.length > 0) {
        for (AVCaptureDevice *dev in devices) {
            if ([dev.uniqueID isEqualToString:_selectedDeviceId]) {
                device = dev;
                break;
            }
        }
    }
    if (!device) {
        device = devices.firstObject;
    }
    NSError *error = nil;
    AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input || error || ![_session canAddInput:input]) {
        _session = nil;
        _syntheticMode = true;
        [self startSyntheticTimerWithWidth:width height:height fps:fps];
        return 0;
    }
    [_session addInput:input];

    AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
    };
    [output setSampleBufferDelegate:self queue:_captureQueue];

    if (![_session canAddOutput:output]) {
        _session = nil;
        _syntheticMode = true;
        [self startSyntheticTimerWithWidth:width height:height fps:fps];
        return 0;
    }
    [_session addOutput:output];

    [_session startRunning];
    return 0;
}

- (void)stop
{
    std::lock_guard<std::recursive_mutex> lock(_driverMutex);
    if (!_running.exchange(false)) return;

    if (_syntheticTimer) {
        dispatch_source_cancel(_syntheticTimer);
        _syntheticTimer = nil;
    }

    if (_session) {
        [_session stopRunning];
        _session = nil;
    }
}

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection
{
    (void)output;
    (void)connection;
    if (!_running.load() || !_enabled.load() || !_frameHandler) {
        return;
    }

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);
    OSType pixelFormat = CVPixelBufferGetPixelFormatType(imageBuffer);

    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        const uint8_t *y_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0));
        size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);

        const uint8_t *uv_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1));
        size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

        size_t u_plane_size = (width / 2) * (height / 2);
        std::vector<uint8_t> u_buffer(u_plane_size);
        std::vector<uint8_t> v_buffer(u_plane_size);

        for (size_t row = 0; row < height / 2; ++row) {
            const uint8_t *uv_row = uv_plane + (row * uv_stride);
            for (size_t col = 0; col < width / 2; ++col) {
                u_buffer[row * (width / 2) + col] = uv_row[col * 2 + 0];
                v_buffer[row * (width / 2) + col] = uv_row[col * 2 + 1];
            }
        }

        sip_video_frame_t frame;
        frame.width = static_cast<int>(width);
        frame.height = static_cast<int>(height);
        frame.y = y_plane;
        frame.u = u_buffer.data();
        frame.v = v_buffer.data();
        frame.y_stride = static_cast<int>(y_stride);
        frame.u_stride = static_cast<int>(width / 2);
        frame.v_stride = static_cast<int>(width / 2);
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        uint64_t ts_ns;
        if (CMTIME_IS_VALID(pts) && pts.timescale > 0) {
            ts_ns = static_cast<uint64_t>(std::max(0.0, (double)pts.value / (double)pts.timescale * 1000000000.0));
        } else {
            ts_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        }
        frame.timestamp_ns = ts_ns;

        _frameHandler(&frame, _frameHandlerArg);
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

// ============================================================================
// MARK: - C ABI Driver Callbacks Glue
// ============================================================================

static int camera_driver_start(void *user_data, int width, int height, int fps)
{
    auto *driver = (__bridge SipMacCameraDriver *)user_data;
    return driver ? [driver startWithWidth:width height:height fps:fps] : -1;
}

static void camera_driver_stop(void *user_data)
{
    auto *driver = (__bridge SipMacCameraDriver *)user_data;
    if (driver) [driver stop];
}

static void camera_driver_set_frame_handler(void *user_data, void (*handler)(const sip_video_frame_t *, void *), void *arg)
{
    auto *driver = (__bridge SipMacCameraDriver *)user_data;
    if (driver) {
        driver->_frameHandler = handler;
        driver->_frameHandlerArg = arg;
    }
}

static void camera_driver_switch_camera(void *user_data) { (void)user_data; }
static void camera_driver_set_camera(void *user_data, int position) { (void)user_data; (void)position; }
static void camera_driver_set_enabled(void *user_data, bool enabled)
{
    auto *driver = (__bridge SipMacCameraDriver *)user_data;
    if (driver) driver->_enabled = enabled;
}

static bool camera_driver_is_enabled(void *user_data)
{
    auto *driver = (__bridge SipMacCameraDriver *)user_data;
    return driver ? driver->_enabled.load() : false;
}

static void remote_renderer_render(void *user_data, const sip_video_frame_t *frame)
{
    auto *client = (__bridge SipMacClient *)user_data;
    if (client && frame) {
        SipMacVideoView *view = client->_remoteVideoView;
        if (view) {
            [view renderYUV420WithY:frame->y
                                  u:frame->u
                                  v:frame->v
                            yStride:frame->y_stride
                            uStride:frame->u_stride
                            vStride:frame->v_stride
                              width:frame->width
                             height:frame->height];
        }
    }
}

static void remote_renderer_on_resized(void *user_data, int width, int height)
{
    (void)user_data; (void)width; (void)height;
}

static void remote_renderer_clear(void *user_data)
{
    auto *client = (__bridge SipMacClient *)user_data;
    if (client && client->_remoteVideoView) {
        [client->_remoteVideoView clear];
    }
}

static void local_renderer_render(void *user_data, const sip_video_frame_t *frame)
{
    auto *client = (__bridge SipMacClient *)user_data;
    if (client && frame) {
        SipMacVideoView *view = client->_localPreviewView;
        if (view) {
            [view renderYUV420WithY:frame->y
                                  u:frame->u
                                  v:frame->v
                            yStride:frame->y_stride
                            uStride:frame->u_stride
                            vStride:frame->v_stride
                              width:frame->width
                             height:frame->height];
        }
    }
}

static void local_renderer_on_resized(void *user_data, int width, int height)
{
    (void)user_data; (void)width; (void)height;
}

static void local_renderer_clear(void *user_data)
{
    auto *client = (__bridge SipMacClient *)user_data;
    if (client && client->_localPreviewView) {
        [client->_localPreviewView clear];
    }
}

// ============================================================================
// MARK: - SipMacClient Implementation
// ============================================================================

static std::string ensure_default_config_dir()
{
    const char *home = std::getenv("HOME");
    std::string base_dir = home ? std::string(home) + "/.sipsdk" : "/tmp/.sipsdk";
    mkdir(base_dir.c_str(), 0755);

    std::string config_file = base_dir + "/config";
    struct stat st;
    if (stat(config_file.c_str(), &st) != 0) {
        std::ofstream ofs(config_file);
        if (ofs.is_open()) {
            ofs << "# Minimal static Baresip configuration for SipSdk Mac GUI\n";
            ofs << "poll_method select\n";
            ofs << "sip_trans_bsize 128\n";
            ofs << "rtp_tos 184\n";
            ofs << "rtcp_enable yes\n";
            ofs << "rtcp_mux no\n";
            ofs << "jitter_buffer_delay 5-10\n";
            ofs << "audio_buffer 80-240\n\n";
            ofs << "audio_player audiounit,nil\n";
            ofs << "audio_source audiounit,nil\n";
            ofs << "module audiounit.so\n";
            ofs << "video_source avcapture,nil\n";
            ofs << "video_display iosview,nil\n";
            ofs << "video_size 640x480\n";
            ofs << "video_fps 30.0\n\n";
            ofs << "module g711.so\n";
            ofs << "module account.so\n";
            ofs << "module stun.so\n";
            ofs << "module turn.so\n";
            ofs << "module ice.so\n";
            ofs << "module srtp.so\n";
        }
    }
    return base_dir;
}

struct ClientCallbacks {
    static void dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data)
    {
        (void)user_data;
        if (task) {
            if ([NSThread isMainThread]) {
                task(arg);
            } else {
                dispatch_async(dispatch_get_main_queue(), ^{
                    task(arg);
                });
            }
        }
    }

    static void on_reg_state(sip_client_t *c, sip_reg_state_t state, int code, const char *reason, void *user_data)
    {
        (void)c;
        auto *self = (__bridge SipMacClient *)user_data;
        if (!self) return;

        NSString *rStr = reason ? [NSString stringWithUTF8String:reason] : @"";
        SipMacRegistrationState regState = static_cast<SipMacRegistrationState>(state);

        dispatch_async(dispatch_get_main_queue(), ^{
            if ([self.delegate respondsToSelector:@selector(client:didChangeRegistrationState:code:reason:)]) {
                [self.delegate client:self didChangeRegistrationState:regState code:code reason:rStr];
            }
        });
    }

    static void on_incoming_call(sip_client_t *c, sip_call_t *call, void *user_data)
    {
        (void)c;
        auto *self = (__bridge SipMacClient *)user_data;
        if (!self || !call) return;

        SipMacCall *macCall = [self getOrCreateCall:call];
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([self.delegate respondsToSelector:@selector(client:didReceiveIncomingCall:)]) {
                [self.delegate client:self didReceiveIncomingCall:macCall];
            }
        });
    }

    static void on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data)
    {
        auto *self = (__bridge SipMacClient *)user_data;
        if (!self || !call) return;

        SipMacCall *macCall = [self getOrCreateCall:call];
        SipMacCallState callState = static_cast<SipMacCallState>(state);
        NSString *rStr = reason ? [NSString stringWithUTF8String:reason] : @"";

        dispatch_async(dispatch_get_main_queue(), ^{
            if ([self.delegate respondsToSelector:@selector(client:call:didChangeState:code:reason:)]) {
                [self.delegate client:self call:macCall didChangeState:callState code:code reason:rStr];
            }
        });

        if (state == SIP_CALL_STATE_CLOSED) {
            [self removeCall:call];
        }
    }

    static void on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data)
    {
        (void)on_hold; (void)is_muted;
        auto *self = (__bridge SipMacClient *)user_data;
        if (!self || !call) return;

        SipMacCall *macCall = [self getOrCreateCall:call];
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([self.delegate respondsToSelector:@selector(client:call:didUpdateMediaWidth:height:)]) {
                [self.delegate client:self call:macCall didUpdateMediaWidth:width height:height];
            }
        });
    }

    static void on_dtmf_digit(sip_call_t *call, const char *digit, void *user_data)
    {
        auto *self = (__bridge SipMacClient *)user_data;
        if (!self || !call) return;

        SipMacCall *macCall = [self getOrCreateCall:call];
        NSString *dStr = digit ? [NSString stringWithUTF8String:digit] : @"";
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([self.delegate respondsToSelector:@selector(client:call:didReceiveDtmf:)]) {
                [self.delegate client:self call:macCall didReceiveDtmf:dStr];
            }
        });
    }
};

// MARK: - CoreAudio Device Helpers

static NSArray<NSDictionary<NSString *, id> *> *get_coreaudio_devices(bool is_input)
{
    NSMutableArray *result = [NSMutableArray array];
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    if (err != noErr || size == 0) return result;

    UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devs(count);
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devs.data());
    if (err != noErr) return result;

    for (UInt32 i = 0; i < count; i++) {
        AudioDeviceID devId = devs[i];

        AudioObjectPropertyAddress streamAddr = {
            kAudioDevicePropertyStreams,
            is_input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 streamSize = 0;
        AudioObjectGetPropertyDataSize(devId, &streamAddr, 0, NULL, &streamSize);
        if (streamSize == 0) continue;

        CFStringRef name = NULL;
        UInt32 nameSz = sizeof(name);
        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        err = AudioObjectGetPropertyData(devId, &nameAddr, 0, NULL, &nameSz, &name);
        if (err != noErr || !name) continue;

        NSString *devName = (__bridge_transfer NSString *)name;
        [result addObject:@{ @"id": @(devId), @"name": devName }];
    }
    return result;
}

static NSString *get_default_audio_device_name(bool is_input)
{
    AudioDeviceID devId = 0;
    UInt32 sz = sizeof(AudioDeviceID);
    AudioObjectPropertyAddress addr = {
        is_input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &sz, &devId);
    if (err != noErr || devId == 0) return nil;

    CFStringRef name = NULL;
    UInt32 nameSz = sizeof(name);
    AudioObjectPropertyAddress nameAddr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    err = AudioObjectGetPropertyData(devId, &nameAddr, 0, NULL, &nameSz, &name);
    return (err == noErr && name) ? (__bridge_transfer NSString *)name : nil;
}

static bool set_default_audio_device_by_name(NSString *targetName, bool is_input)
{
    if (!targetName || targetName.length == 0) return false;
    NSArray<NSDictionary<NSString *, id> *> *devs = get_coreaudio_devices(is_input);
    for (NSDictionary *d in devs) {
        if ([d[@"name"] isEqualToString:targetName]) {
            AudioDeviceID devId = [d[@"id"] unsignedIntValue];
            AudioObjectPropertyAddress addr = {
                is_input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            OSStatus err = AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, sizeof(devId), &devId);
            return (err == noErr);
        }
    }
    return false;
}

@implementation SipMacClient

- (instancetype)initWithUsername:(NSString *)username domain:(NSString *)domain
{
    self = [super init];
    if (self) {
        setlinebuf(stdout);
        setlinebuf(stderr);
        _username = [username copy];
        _domain = [domain copy];
        _port = 5060;
        _transport = @"udp";
        _useSyntheticCamera = NO;
        _cameraDriver = [[SipMacCameraDriver alloc] init];

        _selectedAudioInputDevice = get_default_audio_device_name(true);
        _selectedAudioOutputDevice = get_default_audio_device_name(false);
        _selectedCameraName = @"Synthetic (Test Pattern)";
        _selectedVideoOutputDevice = [NSScreen mainScreen].localizedName ?: @"Main Window Display";
    }
    return self;
}

- (void)dealloc
{
    [self stop];
}

- (NSString *)aor
{
    return [NSString stringWithFormat:@"sip:%@@%@:%d", _username, _domain, _port];
}

- (BOOL)isStarted
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _client != nullptr;
}

- (BOOL)isRegistered
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _client ? sip_client_is_registered(_client) : NO;
}

- (BOOL)isMicMuted
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _client ? sip_client_is_mic_muted(_client) : NO;
}

- (void)setIsMicMuted:(BOOL)isMicMuted
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_client) {
        sip_client_set_mic_muted(_client, isMicMuted);
    }
}

- (int)startWithConfigPath:(nullable NSString *)configPath
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_client) return 0;

    sip_callbacks_t cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.dispatch_to_main = ClientCallbacks::dispatch_to_main;
    cb.on_reg_state = ClientCallbacks::on_reg_state;
    cb.on_incoming_call = ClientCallbacks::on_incoming_call;
    cb.on_call_state = ClientCallbacks::on_call_state;
    cb.on_call_media = ClientCallbacks::on_call_media;
    cb.on_dtmf_digit = ClientCallbacks::on_dtmf_digit;

    _client = sip_client_create(&cb, (__bridge void *)self);
    if (!_client) return -1;

    std::string path = configPath ? [configPath UTF8String] : ensure_default_config_dir();
    int rc = sip_client_init(_client, path.c_str());
    if (rc != 0) {
        sip_client_destroy(_client);
        _client = nullptr;
        return rc;
    }

    // Set AudioUnit driver for macOS
    sip_client_set_audio_driver(_client, "audiounit", "", "audiounit", "");

    // Setup Video Capture Driver
    sip_video_capture_driver_t capDriver;
    std::memset(&capDriver, 0, sizeof(capDriver));
    capDriver.user_data = (__bridge void *)_cameraDriver;
    capDriver.start = camera_driver_start;
    capDriver.stop = camera_driver_stop;
    capDriver.set_frame_handler = camera_driver_set_frame_handler;
    capDriver.switch_camera = camera_driver_switch_camera;
    capDriver.set_camera = camera_driver_set_camera;
    capDriver.set_enabled = camera_driver_set_enabled;
    capDriver.is_enabled = camera_driver_is_enabled;
    sip_client_set_video_capture_driver(_client, &capDriver);

    // Setup Remote Video Renderer
    sip_video_renderer_driver_t remRenderer;
    std::memset(&remRenderer, 0, sizeof(remRenderer));
    remRenderer.user_data = (__bridge void *)self;
    remRenderer.render_frame = remote_renderer_render;
    remRenderer.on_stream_resized = remote_renderer_on_resized;
    remRenderer.clear = remote_renderer_clear;
    sip_client_set_remote_renderer(_client, &remRenderer);

    // Setup Local Video Renderer
    sip_video_renderer_driver_t locRenderer;
    std::memset(&locRenderer, 0, sizeof(locRenderer));
    locRenderer.user_data = (__bridge void *)self;
    locRenderer.render_frame = local_renderer_render;
    locRenderer.on_stream_resized = local_renderer_on_resized;
    locRenderer.clear = local_renderer_clear;
    sip_client_set_local_renderer(_client, &locRenderer);

    return 0;
}

- (void)stop
{
    sip_client_t *c = nullptr;
    std::unordered_map<sip_call_t *, SipMacCall *> calls;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        c = _client;
        _client = nullptr;
        calls = std::move(_activeCalls);
        _activeCalls.clear();
    }
    for (auto &pair : calls) {
        if (pair.second) {
            [pair.second invalidate];
        }
        if (pair.first) {
            sip_call_destroy(pair.first);
        }
    }
    if (c) {
        sip_client_destroy(c);
    }
    if (_cameraDriver) {
        [_cameraDriver stop];
    }
}

- (int)registerWithPassword:(NSString *)password port:(int)port transport:(NSString *)transport
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    _port = port;
    _transport = [transport copy];

    return sip_client_register(_client,
                               [_username UTF8String],
                               [_domain UTF8String],
                               [password UTF8String],
                               [transport UTF8String],
                               port);
}

- (int)registerWithUsername:(NSString *)user
                     domain:(NSString *)domain
                   password:(NSString *)password
                  transport:(NSString *)transport
                       port:(int)port
                displayName:(nullable NSString *)displayName
              outboundProxy:(nullable NSString *)outboundProxy
                 stunServer:(nullable NSString *)stunServer
                   mediaNat:(nullable NSString *)mediaNat
                regInterval:(uint32_t)regInterval
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    _username = [user copy];
    _domain = [domain copy];
    _port = port;
    _transport = [transport copy];

    sip_account_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.user = [user UTF8String];
    cfg.domain = [domain UTF8String];
    cfg.pwd = [password UTF8String];
    cfg.transport = [transport UTF8String];
    cfg.port = port;
    cfg.display_name = displayName.length > 0 ? [displayName UTF8String] : nullptr;
    cfg.outbound_proxy = outboundProxy.length > 0 ? [outboundProxy UTF8String] : nullptr;
    cfg.stun_server = stunServer.length > 0 ? [stunServer UTF8String] : nullptr;
    cfg.media_nat = mediaNat.length > 0 ? [mediaNat UTF8String] : nullptr;
    cfg.reg_interval = regInterval;

    return sip_client_register_account(_client, &cfg);
}

- (int)unregisterAccount
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    return sip_client_unregister(_client);
}

- (int)resetTransports:(BOOL)reRegister
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    return sip_client_reset_transports(_client, reRegister);
}

- (nullable SipMacCall *)makeCallToUri:(NSString *)uri withVideo:(BOOL)withVideo
{
    sip_client_t *c = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        c = _client;
    }
    if (!c || !uri) return nil;

    sip_call_t *h = sip_call_make(c, [uri UTF8String], withVideo);
    if (!h) return nil;

    return [self getOrCreateCall:h];
}

- (void)setRemoteVideoView:(nullable SipMacVideoView *)videoView
{
    _remoteVideoView = videoView;
}

- (void)setLocalPreviewView:(nullable SipMacVideoView *)previewView
{
    _localPreviewView = previewView;
}

- (int)startLocalPreview
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    return sip_client_start_local_preview(_client);
}

- (int)stopLocalPreview
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return -1;
    return sip_client_stop_local_preview(_client);
}

- (BOOL)isLocalPreviewActive
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_client) return NO;
    return sip_client_is_local_preview_active(_client);
}

- (NSArray<NSString *> *)availableAudioInputDevices
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSArray<NSDictionary<NSString *, id> *> *devs = get_coreaudio_devices(true);
    for (NSDictionary *d in devs) {
        NSString *name = d[@"name"];
        if (name && ![names containsObject:name]) {
            [names addObject:name];
        }
    }
    if (names.count == 0) {
        [names addObject:@"Built-in Microphone"];
    }
    return names;
}

- (nullable NSString *)selectedAudioInputDevice
{
    if (_selectedAudioInputDevice.length > 0) {
        return _selectedAudioInputDevice;
    }
    return get_default_audio_device_name(true) ?: @"Built-in Microphone";
}

- (void)selectAudioInputDeviceByName:(NSString *)deviceName
{
    _selectedAudioInputDevice = [deviceName copy];
    set_default_audio_device_by_name(deviceName, true);
    std::lock_guard<std::mutex> lock(_mutex);
    if (_client) {
        sip_client_set_audio_driver(_client, "audiounit", [deviceName UTF8String], "audiounit", nullptr);
    }
}

- (NSArray<NSString *> *)availableAudioOutputDevices
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSArray<NSDictionary<NSString *, id> *> *devs = get_coreaudio_devices(false);
    for (NSDictionary *d in devs) {
        NSString *name = d[@"name"];
        if (name && ![names containsObject:name]) {
            [names addObject:name];
        }
    }
    if (names.count == 0) {
        [names addObject:@"Built-in Output"];
    }
    return names;
}

- (nullable NSString *)selectedAudioOutputDevice
{
    if (_selectedAudioOutputDevice.length > 0) {
        return _selectedAudioOutputDevice;
    }
    return get_default_audio_device_name(false) ?: @"Built-in Output";
}

- (void)selectAudioOutputDeviceByName:(NSString *)deviceName
{
    _selectedAudioOutputDevice = [deviceName copy];
    set_default_audio_device_by_name(deviceName, false);
    std::lock_guard<std::mutex> lock(_mutex);
    if (_client) {
        sip_client_set_audio_driver(_client, "audiounit", nullptr, "audiounit", [deviceName UTF8String]);
    }
}

- (void)useSyntheticCamera:(BOOL)useSynthetic
{
    _useSyntheticCamera = useSynthetic;
    [_cameraDriver setSyntheticMode:useSynthetic];
}

- (NSArray<NSString *> *)availableCameras
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    [names addObject:@"Synthetic (Test Pattern)"];
    for (AVCaptureDevice *dev in [_cameraDriver availableDevices]) {
        [names addObject:dev.localizedName];
    }
    return names;
}

- (nullable NSString *)selectedCamera
{
    return _selectedCameraName ?: @"Synthetic (Test Pattern)";
}

- (void)selectCameraByName:(NSString *)cameraName
{
    _selectedCameraName = [cameraName copy];
    if ([cameraName containsString:@"Synthetic"]) {
        [self useSyntheticCamera:YES];
        return;
    }
    for (AVCaptureDevice *dev in [_cameraDriver availableDevices]) {
        if ([dev.localizedName isEqualToString:cameraName]) {
            [_cameraDriver setCameraDeviceId:dev.uniqueID];
            break;
        }
    }
    [self useSyntheticCamera:NO];
}

- (NSArray<NSString *> *)availableVideoOutputDevices
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSScreen *screen in [NSScreen screens]) {
        [names addObject:screen.localizedName];
    }
    if (names.count == 0) {
        [names addObject:@"Main Window Display"];
    }
    return names;
}

- (nullable NSString *)selectedVideoOutputDevice
{
    if (_selectedVideoOutputDevice.length > 0) {
        return _selectedVideoOutputDevice;
    }
    return [NSScreen mainScreen].localizedName ?: @"Main Window Display";
}

- (void)selectVideoOutputDeviceByName:(NSString *)deviceName
{
    _selectedVideoOutputDevice = [deviceName copy];
}

- (SipMacCall *)getOrCreateCall:(sip_call_t *)handle
{
    if (!handle) return nil;
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _activeCalls.find(handle);
    if (it != _activeCalls.end()) {
        return it->second;
    }
    SipMacCall *call = [[SipMacCall alloc] initWithHandle:handle];
    _activeCalls[handle] = call;
    return call;
}

- (void)removeCall:(sip_call_t *)handle
{
    if (!handle) return;
    SipMacCall *removed = nil;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _activeCalls.find(handle);
        if (it != _activeCalls.end()) {
            removed = it->second;
            _activeCalls.erase(it);
        }
    }
    if (removed) {
        [removed invalidate];
    }
    sip_call_destroy(handle);
}

@end
