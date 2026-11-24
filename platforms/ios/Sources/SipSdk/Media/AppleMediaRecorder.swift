//
//  AppleMediaRecorder.swift
//  SipSdk (iOS / macOS Platform)
//

import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import SipCore

/// Native Apple MP4 muxer (AVAssetWriter) for `SIP_RECORDING_AUDIO_VIDEO`.
/// Accepts 48 kHz S16LE interleaved PCM audio and YUV420P remote video frames
/// from `sip_core` and writes a standard H.264 + AAC `.mp4` file.
public final class AppleMediaRecorder: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.sipsdk.apple-media-recorder")
    private var writer: AVAssetWriter?
    private var audioInput: AVAssetWriterInput?
    private var videoInput: AVAssetWriterInput?
    private var pixelAdaptor: AVAssetWriterInputPixelBufferAdaptor?

    private var outputPath: String = ""
    private var sampleRate: Int = 48000
    private var channels: Int = 2
    private var videoWidth: Int = 0
    private var videoHeight: Int = 0
    private var sessionStarted: Bool = false
    private var isActive: Bool = false
    private var audioFormatDesc: CMAudioFormatDescription?

    public init() {}

    deinit {
        _ = stop()
    }

    public func makeCDriver() -> sip_media_recorder_driver_t {
        let userData = Unmanaged.passUnretained(self).toOpaque()
        return sip_media_recorder_driver_t(
            user_data: userData,
            start: { ud, path, srate, ch, vw, vh in
                guard let ud, let path else { return EINVAL }
                let rec = Unmanaged<AppleMediaRecorder>.fromOpaque(ud).takeUnretainedValue()
                return rec.start(
                    path: String(cString: path),
                    sampleRate: Int(srate),
                    channels: Int(ch),
                    videoWidth: Int(vw),
                    videoHeight: Int(vh)
                )
            },
            write_audio: { ud, pcm, sampleCount, tsNs in
                guard let ud, let pcm, sampleCount > 0 else { return }
                let rec = Unmanaged<AppleMediaRecorder>.fromOpaque(ud).takeUnretainedValue()
                rec.writeAudio(pcm: pcm, sampleCount: sampleCount, timestampNs: tsNs)
            },
            write_video: { ud, framePtr in
                guard let ud, let framePtr else { return }
                let rec = Unmanaged<AppleMediaRecorder>.fromOpaque(ud).takeUnretainedValue()
                rec.writeVideo(frame: framePtr.pointee)
            },
            stop: { ud in
                guard let ud else { return EINVAL }
                let rec = Unmanaged<AppleMediaRecorder>.fromOpaque(ud).takeUnretainedValue()
                return rec.stop()
            }
        )
    }

    public func start(path: String, sampleRate: Int, channels: Int, videoWidth: Int, videoHeight: Int) -> Int32 {
        queue.sync {
            if isActive { return EALREADY }
            let url = URL(fileURLWithPath: path)
            try? FileManager.default.removeItem(at: url)

            guard let assetWriter = try? AVAssetWriter(outputURL: url, fileType: .mp4) else {
                return EIO
            }

            let chCount = max(1, min(2, channels))
            let sr = sampleRate > 0 ? sampleRate : 48000
            let audioSettings: [String: Any] = [
                AVFormatIDKey: kAudioFormatMPEG4AAC,
                AVSampleRateKey: sr,
                AVNumberOfChannelsKey: chCount,
                AVEncoderBitRateKey: 128_000
            ]
            let aInput = AVAssetWriterInput(mediaType: .audio, outputSettings: audioSettings)
            aInput.expectsMediaDataInRealTime = true
            if assetWriter.canAdd(aInput) {
                assetWriter.add(aInput)
            }

            // Create PCM format description for incoming S16LE buffers
            var asbd = AudioStreamBasicDescription(
                mSampleRate: Float64(sr),
                mFormatID: kAudioFormatLinearPCM,
                mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                mBytesPerPacket: UInt32(chCount * 2),
                mFramesPerPacket: 1,
                mBytesPerFrame: UInt32(chCount * 2),
                mChannelsPerFrame: UInt32(chCount),
                mBitsPerChannel: 16,
                mReserved: 0
            )
            var desc: CMAudioFormatDescription?
            CMAudioFormatDescriptionCreate(
                allocator: kCFAllocatorDefault,
                asbd: &asbd,
                layoutSize: 0,
                layout: nil,
                magicCookieSize: 0,
                magicCookie: nil,
                extensions: nil,
                formatDescriptionOut: &desc
            )

            self.writer = assetWriter
            self.audioInput = aInput
            self.audioFormatDesc = desc
            self.videoInput = nil
            self.pixelAdaptor = nil
            self.outputPath = path
            self.sampleRate = sr
            self.channels = chCount
            self.videoWidth = videoWidth
            self.videoHeight = videoHeight
            self.sessionStarted = false
            self.isActive = true

            if videoWidth > 0 && videoHeight > 0 {
                configureVideoInputLocked(width: videoWidth, height: videoHeight)
            }
            return 0
        }
    }

    private func configureVideoInputLocked(width: Int, height: Int) {
        guard let writer = writer, videoInput == nil, writer.status == .unknown else { return }
        let videoSettings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: width,
            AVVideoHeightKey: height
        ]
        let vInput = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        vInput.expectsMediaDataInRealTime = true
        if writer.canAdd(vInput) {
            writer.add(vInput)
            let attrs: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8Planar,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height
            ]
            self.pixelAdaptor = AVAssetWriterInputPixelBufferAdaptor(
                assetWriterInput: vInput,
                sourcePixelBufferAttributes: attrs
            )
            self.videoInput = vInput
            self.videoWidth = width
            self.videoHeight = height
        }
    }

    private func ensureStartedLocked(at time: CMTime) -> Bool {
        guard let writer = writer else { return false }
        if !sessionStarted {
            if writer.status == .unknown {
                guard writer.startWriting() else { return false }
                writer.startSession(atSourceTime: time)
            }
            sessionStarted = true
        }
        return writer.status == .writing
    }

    public func writeAudio(pcm: UnsafePointer<Int16>, sampleCount: Int, timestampNs: UInt64) {
        let byteCount = sampleCount * MemoryLayout<Int16>.size
        let copy = Data(bytes: pcm, count: byteCount)
        queue.sync {
            guard isActive, let aInput = audioInput, let desc = audioFormatDesc else { return }
            let frames = sampleCount / max(1, channels)
            guard frames > 0 else { return }

            let pts = CMTime(value: Int64(timestampNs), timescale: 1_000_000_000)
            guard ensureStartedLocked(at: pts), aInput.isReadyForMoreMediaData else { return }

            var blockBuffer: CMBlockBuffer?
            let status = CMBlockBufferCreateWithMemoryBlock(
                allocator: kCFAllocatorDefault,
                memoryBlock: nil,
                blockLength: byteCount,
                blockAllocator: kCFAllocatorDefault,
                customBlockSource: nil,
                offsetToData: 0,
                dataLength: byteCount,
                flags: 0,
                blockBufferOut: &blockBuffer
            )
            guard status == kCMBlockBufferNoErr, let bb = blockBuffer else { return }
            copy.withUnsafeBytes { raw in
                if let base = raw.baseAddress {
                    CMBlockBufferReplaceDataBytes(with: base, blockBuffer: bb, offsetIntoDestination: 0, dataLength: byteCount)
                }
            }

            var timing = CMSampleTimingInfo(
                duration: CMTime(value: Int64(frames), timescale: CMTimeScale(sampleRate)),
                presentationTimeStamp: pts,
                decodeTimeStamp: .invalid
            )
            var sampleBuffer: CMSampleBuffer?
            var sampleSize = byteCount
            let sbStatus = CMSampleBufferCreate(
                allocator: kCFAllocatorDefault,
                dataBuffer: bb,
                dataReady: true,
                makeDataReadyCallback: nil,
                refcon: nil,
                formatDescription: desc,
                sampleCount: frames,
                sampleTimingEntryCount: 1,
                sampleTimingArray: &timing,
                sampleSizeEntryCount: 1,
                sampleSizeArray: &sampleSize,
                sampleBufferOut: &sampleBuffer
            )
            if sbStatus == noErr, let sb = sampleBuffer {
                aInput.append(sb)
            }
        }
    }

    public func writeVideo(frame: sip_video_frame_t) {
        guard let yPtr = frame.y, let uPtr = frame.u, let vPtr = frame.v,
              frame.width > 0, frame.height > 0 else { return }
        let w = Int(frame.width)
        let h = Int(frame.height)
        let yStride = Int(frame.y_stride)
        let uStride = Int(frame.u_stride)
        let vStride = Int(frame.v_stride)
        let tsNs = frame.timestamp_ns

        queue.sync {
            guard isActive else { return }
            if videoInput == nil {
                configureVideoInputLocked(width: w, height: h)
            }
            guard let vInput = videoInput, let adaptor = pixelAdaptor else { return }
            let pts = CMTime(value: Int64(tsNs), timescale: 1_000_000_000)
            guard ensureStartedLocked(at: pts), vInput.isReadyForMoreMediaData else { return }

            var pb: CVPixelBuffer?
            if let pool = adaptor.pixelBufferPool {
                CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb)
            } else {
                CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_420YpCbCr8Planar, nil, &pb)
            }
            guard let pixelBuffer = pb else { return }

            CVPixelBufferLockBaseAddress(pixelBuffer, [])
            defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }

            if let dstY = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0) {
                let dstStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)
                for row in 0..<h {
                    memcpy(dstY.advanced(by: row * dstStride), yPtr.advanced(by: row * yStride), min(w, dstStride))
                }
            }
            let uvW = w / 2
            let uvH = h / 2
            if let dstU = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1) {
                let dstStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1)
                for row in 0..<uvH {
                    memcpy(dstU.advanced(by: row * dstStride), uPtr.advanced(by: row * uStride), min(uvW, dstStride))
                }
            }
            if let dstV = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 2) {
                let dstStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 2)
                for row in 0..<uvH {
                    memcpy(dstV.advanced(by: row * dstStride), vPtr.advanced(by: row * vStride), min(uvW, dstStride))
                }
            }

            adaptor.append(pixelBuffer, withPresentationTime: pts)
        }
    }

    @discardableResult
    public func stop() -> Int32 {
        queue.sync {
            guard isActive else { return 0 }
            isActive = false
            let activeWriter = writer
            let aInput = audioInput
            let vInput = videoInput
            let started = sessionStarted

            writer = nil
            audioInput = nil
            videoInput = nil
            pixelAdaptor = nil
            audioFormatDesc = nil
            sessionStarted = false

            guard let w = activeWriter else { return 0 }
            if started && w.status == .writing {
                aInput?.markAsFinished()
                vInput?.markAsFinished()
                let sem = DispatchSemaphore(value: 0)
                w.finishWriting {
                    sem.signal()
                }
                _ = sem.wait(timeout: .now() + 5.0)
            } else {
                w.cancelWriting()
            }
            return 0
        }
    }
}
