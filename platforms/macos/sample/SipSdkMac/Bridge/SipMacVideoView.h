#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface SipMacVideoView : NSView

@property (nonatomic, readonly) CGSize videoSize;
@property (nonatomic, readonly) NSUInteger renderedFrameCount;

- (void)renderYUV420WithY:(const uint8_t *)yPlane
                        u:(const uint8_t *)uPlane
                        v:(const uint8_t *)vPlane
                  yStride:(int)yStride
                  uStride:(int)uStride
                  vStride:(int)vStride
                    width:(int)width
                   height:(int)height;

- (void)clear;

@end

NS_ASSUME_NONNULL_END
