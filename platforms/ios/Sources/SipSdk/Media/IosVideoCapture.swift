//
//  IosVideoCapture.swift
//  SipSdk (iOS Platform)
//

import Foundation
import AVFoundation
import CoreMedia
import CoreVideo
import SipCore

/// Native iOS video capture driver bridging AVCaptureSession and synthetic generator to `sip_video_capture_driver_t`.
public final class IosVideoCaptureDriver: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var captureSession: AVCaptureSession?
    private var videoOutput: AVCaptureVideoDataOutput?
    private var videoDeviceInput: AVCaptureDeviceInput?
    private var syntheticTimer: DispatchSourceTimer?
    private let captureQueue = DispatchQueue(label: "com.sipsdk.ios.videocapture", qos: .userInteractive)

    private var frameHandler: (@convention(c) (UnsafePointer<sip_video_frame_t>?, UnsafeMutableRawPointer?) -> Void)?
    private var frameHandlerArg: UnsafeMutableRawPointer?

    public private(set) var isRunning: Bool = false
    public private(set) var isEnabled: Bool = true
    public private(set) var currentPosition: SipCameraPosition = .front
    public private(set) var isLiveCameraFeedActive: Bool = false

    private var targetWidth: Int = 640
    private var targetHeight: Int = 480
    private var targetFps: Int = 30

    private var uChromaBuffer: [UInt8] = []
    private var vChromaBuffer: [UInt8] = []

    public override init() {
        super.init()
    }

    deinit {
        stop()
    }

    /// Creates the C driver struct with function pointers.
    public func makeCDriver() -> sip_video_capture_driver_t {
        var driver = sip_video_capture_driver_t()
        driver.user_data = Unmanaged.passUnretained(self).toOpaque()

        driver.start = { userData, width, height, fps in
            guard let userData = userData else { return -1 }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            return Int32(cap.start(width: Int(width), height: Int(height), fps: Int(fps)))
        }

        driver.stop = { userData in
            guard let userData = userData else { return }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            cap.stop()
        }

        driver.set_frame_handler = { userData, handler, arg in
            guard let userData = userData else { return }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            cap.setFrameHandler(handler, arg: arg)
        }

        driver.switch_camera = { userData in
            guard let userData = userData else { return }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            cap.switchCamera()
        }

        driver.set_camera = { userData, position in
            guard let userData = userData else { return }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            cap.setCamera(position == 0 ? .front : .back)
        }

        driver.set_enabled = { userData, enabled in
            guard let userData = userData else { return }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            cap.setEnabled(enabled)
        }

        driver.is_enabled = { userData in
            guard let userData = userData else { return false }
            let cap = Unmanaged<IosVideoCaptureDriver>.fromOpaque(userData).takeUnretainedValue()
            return cap.isEnabled
        }

        return driver
    }

    public func setFrameHandler(
        _ handler: (@convention(c) (UnsafePointer<sip_video_frame_t>?, UnsafeMutableRawPointer?) -> Void)?,
        arg: UnsafeMutableRawPointer?
    ) {
        lock.lock()
        frameHandler = handler
        frameHandlerArg = arg
        lock.unlock()
    }

    public func start(width: Int, height: Int, fps: Int) -> Int {
        lock.lock()
        defer { lock.unlock() }

        targetWidth = width > 0 ? width : 640
        targetHeight = height > 0 ? height : 480
        targetFps = fps > 0 ? fps : 30
        isRunning = true

        #if targetEnvironment(simulator)
        startSyntheticTimer()
        return 0
        #else
        let status = AVCaptureDevice.authorizationStatus(for: .video)
        if status == .authorized {
            if startHardwareCamera() {
                return 0
            }
        } else if status == .notDetermined {
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                guard let self = self, granted else { return }
                self.lock.lock()
                if self.isRunning && !self.isLiveCameraFeedActive {
                    _ = self.startHardwareCamera()
                }
                self.lock.unlock()
            }
        }

        startSyntheticTimer()
        return 0
        #endif
    }

    public func stop() {
        lock.lock()
        defer { lock.unlock() }

        isRunning = false
        stopSyntheticTimer()
        stopHardwareCamera()
    }

    public func switchCamera() {
        setCamera(currentPosition == .front ? .back : .front)
    }

    public func setCamera(_ position: SipCameraPosition) {
        lock.lock()
        defer { lock.unlock() }

        guard currentPosition != position else { return }
        currentPosition = position

        #if !targetEnvironment(simulator)
        if isLiveCameraFeedActive {
            stopHardwareCamera()
            _ = startHardwareCamera()
        }
        #endif
    }

    public func setEnabled(_ enabled: Bool) {
        lock.lock()
        defer { lock.unlock() }
        isEnabled = enabled
    }

    // MARK: - Hardware Camera

    private func startHardwareCamera() -> Bool {
        let devicePosition: AVCaptureDevice.Position = (currentPosition == .front) ? .front : .back
        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: devicePosition) else {
            return false
        }

        do {
            let input = try AVCaptureDeviceInput(device: device)
            let session = AVCaptureSession()
            session.beginConfiguration()
            session.sessionPreset = .vga640x480

            if session.canAddInput(input) {
                session.addInput(input)
                self.videoDeviceInput = input
            } else {
                return false
            }

            let output = AVCaptureVideoDataOutput()
            output.alwaysDiscardsLateVideoFrames = true

            let formatTypes: [OSType]
            if let numbers = output.value(forKey: "availableVideoCVPixelFormatTypes") as? [NSNumber] {
                formatTypes = numbers.map { $0.uint32Value }
            } else {
                formatTypes = []
            }

            let chosenFormat: OSType
            if formatTypes.contains(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
                chosenFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            } else if formatTypes.contains(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
                chosenFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            } else if let first = formatTypes.first {
                chosenFormat = first
            } else {
                chosenFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            }

            output.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: chosenFormat
            ]
            output.setSampleBufferDelegate(self, queue: captureQueue)

            if session.canAddOutput(output) {
                session.addOutput(output)
                self.videoOutput = output
            } else {
                return false
            }

            if let connection = output.connection(with: .video) {
                if #available(iOS 17.0, *) {
                    if connection.isVideoRotationAngleSupported(90) {
                        connection.videoRotationAngle = 90
                    }
                } else {
                    if connection.isVideoOrientationSupported {
                        connection.videoOrientation = .portrait
                    }
                }
                if connection.isVideoMirroringSupported {
                    connection.isVideoMirrored = (devicePosition == .front)
                }
            }

            session.commitConfiguration()
            session.startRunning()

            self.captureSession = session
            self.isLiveCameraFeedActive = true
            stopSyntheticTimer()
            return true
        } catch {
            NSLog("[SipSdk] Failed to start AVCaptureSession: %@", error.localizedDescription)
            return false
        }
    }

    private func stopHardwareCamera() {
        captureSession?.stopRunning()
        captureSession = nil
        videoOutput = nil
        videoDeviceInput = nil
        isLiveCameraFeedActive = false
        uChromaBuffer.removeAll(keepingCapacity: true)
        vChromaBuffer.removeAll(keepingCapacity: true)
    }

    // MARK: - AVCaptureVideoDataOutputSampleBufferDelegate

    public func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard isEnabled, isRunning else { return }
        guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        CVPixelBufferLockBaseAddress(imageBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(imageBuffer, .readOnly) }

        let width = CVPixelBufferGetWidth(imageBuffer)
        let height = CVPixelBufferGetHeight(imageBuffer)
        let planeCount = CVPixelBufferGetPlaneCount(imageBuffer)

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let ts: UInt64
        if pts.isValid && pts.timescale > 0 {
            ts = UInt64(max(0, Double(pts.value) / Double(pts.timescale) * 1_000_000_000.0))
        } else {
            ts = UInt64(mach_absolute_time())
        }

        if planeCount >= 3 {
            guard let yPlane = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0),
                  let uPlane = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1),
                  let vPlane = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 2) else { return }

            let yStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0)
            let uStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1)
            let vStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 2)

            var frame = sip_video_frame_t(
                width: Int32(width),
                height: Int32(height),
                y: yPlane.assumingMemoryBound(to: UInt8.self),
                u: uPlane.assumingMemoryBound(to: UInt8.self),
                v: vPlane.assumingMemoryBound(to: UInt8.self),
                y_stride: Int32(yStride),
                u_stride: Int32(uStride),
                v_stride: Int32(vStride),
                timestamp_ns: ts
            )

            lock.lock()
            let handler = frameHandler
            let arg = frameHandlerArg
            lock.unlock()

            handler?(&frame, arg)
        } else if planeCount == 2 {
            guard let yPlane = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0),
                  let uvPlane = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1) else { return }

            let yStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0)
            let uvStride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1)
            let halfWidth = width / 2
            let halfHeight = height / 2
            let chromaSize = halfWidth * halfHeight

            if uChromaBuffer.count != chromaSize {
                uChromaBuffer = [UInt8](repeating: 128, count: chromaSize)
                vChromaBuffer = [UInt8](repeating: 128, count: chromaSize)
            }

            let uvRaw = uvPlane.assumingMemoryBound(to: UInt8.self)
            uChromaBuffer.withUnsafeMutableBufferPointer { uPtr in
                vChromaBuffer.withUnsafeMutableBufferPointer { vPtr in
                    guard let uBase = uPtr.baseAddress, let vBase = vPtr.baseAddress else { return }
                    for row in 0..<halfHeight {
                        let uvRow = uvRaw.advanced(by: row * uvStride)
                        let dstRowOffset = row * halfWidth
                        for col in 0..<halfWidth {
                            uBase[dstRowOffset + col] = uvRow[col * 2 + 0]
                            vBase[dstRowOffset + col] = uvRow[col * 2 + 1]
                        }
                    }

                    var frame = sip_video_frame_t(
                        width: Int32(width),
                        height: Int32(height),
                        y: yPlane.assumingMemoryBound(to: UInt8.self),
                        u: uBase,
                        v: vBase,
                        y_stride: Int32(yStride),
                        u_stride: Int32(halfWidth),
                        v_stride: Int32(halfWidth),
                        timestamp_ns: ts
                    )

                    lock.lock()
                    let handler = frameHandler
                    let arg = frameHandlerArg
                    lock.unlock()

                    handler?(&frame, arg)
                }
            }
        }
    }

    // MARK: - Synthetic Fallback Generator

    private func startSyntheticTimer() {
        guard syntheticTimer == nil else { return }

        let timer = DispatchSource.makeTimerSource(queue: captureQueue)
        let intervalMs = max(10, 1000 / targetFps)
        timer.schedule(deadline: .now(), repeating: .milliseconds(intervalMs))
        timer.setEventHandler { [weak self] in
            self?.deliverSyntheticFrame()
        }
        timer.resume()
        syntheticTimer = timer
    }

    private func stopSyntheticTimer() {
        syntheticTimer?.cancel()
        syntheticTimer = nil
    }

    public func deliverSyntheticFrame() {
        guard isEnabled, isRunning else { return }

        let w = targetWidth
        let h = targetHeight
        let yStride = w
        let uStride = w / 2
        let vStride = w / 2

        let ySize = yStride * h
        let uSize = uStride * (h / 2)
        let vSize = vStride * (h / 2)

        let yPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: ySize)
        let uPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: uSize)
        let vPlane = UnsafeMutablePointer<UInt8>.allocate(capacity: vSize)

        defer {
            yPlane.deallocate()
            uPlane.deallocate()
            vPlane.deallocate()
        }

        let barWidth = w / 8
        let barY: [UInt8] = [235, 210, 170, 145, 106, 81, 41, 16]
        let barU: [UInt8] = [128, 16, 166, 54, 202, 90, 240, 128]
        let barV: [UInt8] = [128, 146, 16, 34, 222, 240, 110, 128]

        for y in 0..<h {
            for x in 0..<w {
                let bar = min(7, x / max(1, barWidth))
                yPlane[y * yStride + x] = barY[bar]
            }
        }

        for y in 0..<(h / 2) {
            for x in 0..<(w / 2) {
                let bar = min(7, (x * 2) / max(1, barWidth))
                uPlane[y * uStride + x] = barU[bar]
                vPlane[y * vStride + x] = barV[bar]
            }
        }

        var frame = sip_video_frame_t(
            width: Int32(w),
            height: Int32(h),
            y: yPlane,
            u: uPlane,
            v: vPlane,
            y_stride: Int32(yStride),
            u_stride: Int32(uStride),
            v_stride: Int32(vStride),
            timestamp_ns: UInt64(mach_absolute_time())
        )

        lock.lock()
        let handler = frameHandler
        let arg = frameHandlerArg
        lock.unlock()

        handler?(&frame, arg)
    }
}
