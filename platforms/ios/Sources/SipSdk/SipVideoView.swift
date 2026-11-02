//
//  SipVideoView.swift
//  SipSdk (iOS Platform)
//

import UIKit
import SwiftUI
import CoreGraphics
import QuartzCore

/// Turnkey UIKit view for hardware-accelerated SIP video rendering.
public final class SipVideoUIView: UIView {
    public private(set) var videoSize: CGSize = .zero
    public private(set) var renderedFrameCount: Int = 0

    /// Mirrors video horizontally (useful for local front camera preview).
    public var isMirrored: Bool = false {
        didSet {
            updateTransform()
        }
    }

    public override init(frame: CGRect) {
        super.init(frame: frame)
        commonInit()
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        commonInit()
    }

    private func commonInit() {
        backgroundColor = .black
        layer.backgroundColor = UIColor.black.cgColor
        layer.contentsGravity = .resizeAspectFill
        layer.masksToBounds = true
        contentMode = .scaleAspectFill
    }

    private func updateTransform() {
        if isMirrored {
            layer.transform = CATransform3DMakeScale(-1.0, 1.0, 1.0)
        } else {
            layer.transform = CATransform3DIdentity
        }
    }

    @inline(__always)
    private static func clampByte(_ v: Int) -> UInt8 {
        return UInt8(max(0, min(255, v)))
    }

    /// Renders incoming decoded YUV420P frame buffers directly into the view layer.
    public func renderYUV420(
        yPlane: UnsafePointer<UInt8>,
        uPlane: UnsafePointer<UInt8>,
        vPlane: UnsafePointer<UInt8>,
        yStride: Int,
        uStride: Int,
        vStride: Int,
        width: Int,
        height: Int
    ) {
        guard width > 0, height > 0 else { return }

        let rgbaLen = width * height * 4
        let rgba = UnsafeMutablePointer<UInt8>.allocate(capacity: rgbaLen)

        for y in 0..<height {
            let yRow = yPlane.advanced(by: y * yStride)
            let uRow = uPlane.advanced(by: (y / 2) * uStride)
            let vRow = vPlane.advanced(by: (y / 2) * vStride)
            let dstRow = rgba.advanced(by: y * width * 4)

            for x in 0..<width {
                let Y = max(0, Int(yRow[x]) - 16)
                let U = Int(uRow[x / 2]) - 128
                let V = Int(vRow[x / 2]) - 128

                let c = 298 * Y
                let r = (c + 409 * V + 128) >> 8
                let g = (c - 100 * U - 208 * V + 128) >> 8
                let b = (c + 516 * U + 128) >> 8

                dstRow[x * 4 + 0] = Self.clampByte(r)
                dstRow[x * 4 + 1] = Self.clampByte(g)
                dstRow[x * 4 + 2] = Self.clampByte(b)
                dstRow[x * 4 + 3] = 255
            }
        }

        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let provider = CGDataProvider(
            dataInfo: nil,
            data: rgba,
            size: rgbaLen,
            releaseData: { _, data, _ in
                data.deallocate()
            }
        ) else {
            rgba.deallocate()
            return
        }

        guard let cgImage = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else {
            return
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.videoSize = CGSize(width: width, height: height)
            self.renderedFrameCount += 1
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            self.layer.contents = cgImage
            CATransaction.commit()
        }
    }

    /// Renders a synthetic test pattern (used for camera preview testing or simulator).
    public func renderTestPattern(width: Int = 640, height: Int = 480) {
        let yStride = width
        let uStride = width / 2
        let vStride = width / 2

        let ySize = yStride * height
        let uSize = uStride * (height / 2)
        let vSize = vStride * (height / 2)

        let yPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: ySize)
        let uPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: uSize)
        let vPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: vSize)

        defer {
            yPlane.deallocate()
            uPlane.deallocate()
            vPlane.deallocate()
        }

        let barWidth = width / 8
        let barY: [UInt8] = [235, 210, 170, 145, 106, 81, 41, 16]
        let barU: [UInt8] = [128, 16, 166, 54, 202, 90, 240, 128]
        let barV: [UInt8] = [128, 146, 16, 34, 222, 240, 110, 128]

        for y in 0..<height {
            for x in 0..<width {
                let bar = min(7, x / max(1, barWidth))
                yPlane[y * yStride + x] = barY[bar]
            }
        }

        for y in 0..<(height / 2) {
            for x in 0..<(width / 2) {
                let bar = min(7, (x * 2) / max(1, barWidth))
                uPlane[y * uStride + x] = barU[bar]
                vPlane[y * vStride + x] = barV[bar]
            }
        }

        renderYUV420(
            yPlane: yPlane,
            uPlane: uPlane,
            vPlane: vPlane,
            yStride: yStride,
            uStride: uStride,
            vStride: vStride,
            width: width,
            height: height
        )
    }

    /// Clears video contents back to black.
    public func clearVideo() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            self.layer.contents = nil
            CATransaction.commit()
            self.videoSize = .zero
        }
    }
}

/// Turnkey SwiftUI Representable wrapping `SipVideoUIView`.
public struct SipVideoView: UIViewRepresentable {
    public let videoUIView: SipVideoUIView

    public init(videoUIView: SipVideoUIView) {
        self.videoUIView = videoUIView
    }

    public func makeUIView(context: Context) -> SipVideoUIView {
        return videoUIView
    }

    public func updateUIView(_ uiView: SipVideoUIView, context: Context) {
        // Dynamic layout adjustments if needed
    }
}
