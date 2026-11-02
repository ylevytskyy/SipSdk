#import "SipMacVideoView.h"
#import <QuartzCore/QuartzCore.h>
#include <cstdlib>

@interface SipMacVideoView ()
@property (nonatomic, readwrite) CGSize videoSize;
@property (nonatomic, readwrite) NSUInteger renderedFrameCount;
@end

@implementation SipMacVideoView

- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder
{
    self = [super initWithCoder:coder];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (void)commonInit
{
    self.wantsLayer = YES;
    self.layer = [CALayer layer];
    self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    self.layer.contentsGravity = kCAGravityResizeAspectFill;
    self.layer.masksToBounds = YES;
    _videoSize = CGSizeZero;
    _renderedFrameCount = 0;
}

static inline uint8_t clampByte(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

- (void)renderYUV420WithY:(const uint8_t *)yPlane
                        u:(const uint8_t *)uPlane
                        v:(const uint8_t *)vPlane
                  yStride:(int)yStride
                  uStride:(int)uStride
                  vStride:(int)vStride
                    width:(int)width
                   height:(int)height
{
    if (!yPlane || !uPlane || !vPlane || width <= 0 || height <= 0) {
        return;
    }

    size_t rgbaLen = (size_t)width * (size_t)height * 4;
    uint8_t *rgba = (uint8_t *)malloc(rgbaLen);
    if (!rgba) {
        return;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *yRow = yPlane + y * yStride;
        const uint8_t *uRow = uPlane + (y / 2) * uStride;
        const uint8_t *vRow = vPlane + (y / 2) * vStride;
        uint8_t *dstRow = rgba + (size_t)y * (size_t)width * 4;

        for (int x = 0; x < width; x++) {
            int Y = (int)yRow[x] - 16;
            int U = (int)uRow[x / 2] - 128;
            int V = (int)vRow[x / 2] - 128;
            if (Y < 0) Y = 0;

            int c = 298 * Y;
            int r = (c + 409 * V + 128) >> 8;
            int g = (c - 100 * U - 208 * V + 128) >> 8;
            int b = (c + 516 * U + 128) >> 8;

            dstRow[x * 4 + 0] = clampByte(r);
            dstRow[x * 4 + 1] = clampByte(g);
            dstRow[x * 4 + 2] = clampByte(b);
            dstRow[x * 4 + 3] = 255;
        }
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, rgba, rgbaLen,
        [](void *info, const void *data, size_t size) {
            free((void *)data);
        });

    CGImageRef cgImage = CGImageCreate(
        (size_t)width, (size_t)height,
        8, 32, (size_t)width * 4,
        colorSpace,
        kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast,
        provider, NULL, false, kCGRenderingIntentDefault);

    CGDataProviderRelease(provider);
    CGColorSpaceRelease(colorSpace);

    if (!cgImage) {
        return;
    }

    self.videoSize = CGSizeMake(width, height);
    self.renderedFrameCount += 1;

    if ([NSThread isMainThread]) {
        self.layer.contents = (__bridge id)cgImage;
        CGImageRelease(cgImage);
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.layer.contents = (__bridge id)cgImage;
            CGImageRelease(cgImage);
        });
    }
}

- (void)clear
{
    _videoSize = CGSizeZero;
    if ([NSThread isMainThread]) {
        self.layer.contents = nil;
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.layer.contents = nil;
        });
    }
}

@end
