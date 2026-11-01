#include <sip/video_driver.hpp>

#if defined(__APPLE__)
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

#include <atomic>
#include <mutex>
#include <vector>

@interface SipDesktopAVFCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) sip_core::FrameHandler frameHandler;
@property (nonatomic, assign) std::atomic<bool> *enabledPtr;
@end

@implementation SipDesktopAVFCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection *)connection {
    (void)output;
    (void)connection;

    if (!self.enabledPtr || !self.enabledPtr->load() || !self.frameHandler) {
        return;
    }

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);

    sip_core::VideoFrame vf;
    vf.width = static_cast<int>(width);
    vf.height = static_cast<int>(height);

    OSType pixelFormat = CVPixelBufferGetPixelFormatType(imageBuffer);
    std::vector<uint8_t> yuv420_conversion;

    if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        // NV12 format: Plane 0 = Y, Plane 1 = interleaved UV
        const uint8_t *y_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0));
        size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);

        const uint8_t *uv_plane = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1));
        size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

        size_t u_plane_size = (width / 2) * (height / 2);
        yuv420_conversion.resize(u_plane_size * 2);
        uint8_t *u_dst = yuv420_conversion.data();
        uint8_t *v_dst = u_dst + u_plane_size;

        for (size_t row = 0; row < height / 2; ++row) {
            const uint8_t *uv_row = uv_plane + (row * uv_stride);
            for (size_t col = 0; col < width / 2; ++col) {
                u_dst[row * (width / 2) + col] = uv_row[col * 2];
                v_dst[row * (width / 2) + col] = uv_row[col * 2 + 1];
            }
        }

        vf.y = y_plane;
        vf.u = u_dst;
        vf.v = v_dst;
        vf.y_stride = static_cast<int>(y_stride);
        vf.u_stride = static_cast<int>(width / 2);
        vf.v_stride = static_cast<int>(width / 2);
    } else {
        // Planar YUV420 or direct buffer
        vf.y = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(imageBuffer));
        vf.y_stride = static_cast<int>(CVPixelBufferGetBytesPerRow(imageBuffer));
        vf.u = vf.y + (vf.y_stride * height);
        vf.v = vf.u + ((vf.y_stride / 2) * (height / 2));
        vf.u_stride = vf.y_stride / 2;
        vf.v_stride = vf.y_stride / 2;
    }

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    vf.timestamp_ns = static_cast<uint64_t>(CMTimeGetSeconds(pts) * 1e9);

    if (self.frameHandler) {
        self.frameHandler(vf);
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

namespace sip {

struct AVFoundationVideoCaptureDriver::Impl {
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{true};
    std::atomic<int> camera_position{0};
    sip_core::FrameHandler frame_handler;
    std::mutex mutex;

    AVCaptureSession *session{nil};
    SipDesktopAVFCaptureDelegate *delegate{nil};
    dispatch_queue_t queue{nil};

    ~Impl() {
        stop_capture();
    }

    int start_capture(int width, int height, int fps) {
        (void)width;
        (void)height;
        (void)fps;
        std::lock_guard<std::mutex> lock(mutex);
        if (running.load()) return 0;

        @autoreleasepool {
            session = [[AVCaptureSession alloc] init];
            if ([session canSetSessionPreset:AVCaptureSessionPreset640x480]) {
                session.sessionPreset = AVCaptureSessionPreset640x480;
            }

            AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
            if (!device) {
                session = nil;
                return -1;
            }

            NSError *error = nil;
            AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
            if (!input || error || ![session canAddInput:input]) {
                session = nil;
                return -1;
            }
            [session addInput:input];

            AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
            output.alwaysDiscardsLateVideoFrames = YES;
            output.videoSettings = @{
                (NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
            };

            delegate = [[SipDesktopAVFCaptureDelegate alloc] init];
            delegate.frameHandler = frame_handler;
            delegate.enabledPtr = &enabled;

            queue = dispatch_queue_create("com.sipsdk.desktop.avf_capture", DISPATCH_QUEUE_SERIAL);
            [output setSampleBufferDelegate:delegate queue:queue];

            if (![session canAddOutput:output]) {
                session = nil;
                delegate = nil;
                return -1;
            }
            [session addOutput:output];

            [session startRunning];
            running = true;
        }
        return 0;
    }

    void stop_capture() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running.exchange(false)) return;

        @autoreleasepool {
            if (session) {
                [session stopRunning];
                session = nil;
            }
            delegate = nil;
            queue = nil;
        }
    }
};

AVFoundationVideoCaptureDriver::AVFoundationVideoCaptureDriver()
    : m_impl(std::make_unique<Impl>())
{
}

AVFoundationVideoCaptureDriver::~AVFoundationVideoCaptureDriver() = default;

int AVFoundationVideoCaptureDriver::start(int width, int height, int fps)
{
    return m_impl->start_capture(width, height, fps);
}

void AVFoundationVideoCaptureDriver::stop()
{
    m_impl->stop_capture();
}

void AVFoundationVideoCaptureDriver::set_frame_handler(sip_core::FrameHandler handler)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->frame_handler = std::move(handler);
    if (m_impl->delegate) {
        m_impl->delegate.frameHandler = m_impl->frame_handler;
    }
}

void AVFoundationVideoCaptureDriver::switch_camera()
{
    m_impl->camera_position = (m_impl->camera_position == 0) ? 1 : 0;
}

void AVFoundationVideoCaptureDriver::set_camera(int position)
{
    m_impl->camera_position = position;
}

void AVFoundationVideoCaptureDriver::set_enabled(bool enabled)
{
    m_impl->enabled = enabled;
}

bool AVFoundationVideoCaptureDriver::is_enabled() const
{
    return m_impl->enabled.load();
}

std::shared_ptr<sip_core::IVideoCaptureDriver> create_default_camera_capture_driver()
{
    return std::make_shared<AVFoundationVideoCaptureDriver>();
}

} // namespace sip
#endif
