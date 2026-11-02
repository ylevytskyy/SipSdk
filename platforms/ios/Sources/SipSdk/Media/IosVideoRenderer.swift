//
//  IosVideoRenderer.swift
//  SipSdk (iOS Platform)
//

import UIKit
import SipCore

/// Bridges the C ABI `sip_video_renderer_driver_t` to `SipVideoUIView`.
public final class IosVideoRendererDriver: @unchecked Sendable {
    public weak var videoView: SipVideoUIView?

    public init(videoView: SipVideoUIView? = nil) {
        self.videoView = videoView
    }

    /// Creates the C driver struct with function pointers.
    public func makeCDriver() -> sip_video_renderer_driver_t {
        var driver = sip_video_renderer_driver_t()
        driver.user_data = Unmanaged.passUnretained(self).toOpaque()

        driver.render_frame = { userData, framePtr in
            guard let userData = userData, let frame = framePtr?.pointee else { return }
            let renderer = Unmanaged<IosVideoRendererDriver>.fromOpaque(userData).takeUnretainedValue()
            renderer.render(frame: frame)
        }

        driver.on_stream_resized = { userData, width, height in
            guard let userData = userData else { return }
            let renderer = Unmanaged<IosVideoRendererDriver>.fromOpaque(userData).takeUnretainedValue()
            renderer.onStreamResized(width: Int(width), height: Int(height))
        }

        driver.clear = { userData in
            guard let userData = userData else { return }
            let renderer = Unmanaged<IosVideoRendererDriver>.fromOpaque(userData).takeUnretainedValue()
            renderer.clear()
        }

        return driver
    }

    private func render(frame: sip_video_frame_t) {
        guard let view = videoView,
              let y = frame.y,
              let u = frame.u,
              let v = frame.v else { return }

        view.renderYUV420(
            yPlane: y,
            uPlane: u,
            vPlane: v,
            yStride: Int(frame.y_stride),
            uStride: Int(frame.u_stride),
            vStride: Int(frame.v_stride),
            width: Int(frame.width),
            height: Int(frame.height)
        )
    }

    private func onStreamResized(width: Int, height: Int) {
        // Stream size updated
    }

    private func clear() {
        videoView?.clearVideo()
    }
}
