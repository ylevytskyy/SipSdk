//
//  CallRecordingTests.swift
//  SipSdkTests
//

import AVFoundation
import Foundation
import SipCore
import XCTest
@testable import SipSdk

final class CallRecordingTests: XCTestCase {
    func testAppleMediaRecorderWritesPlayableMp4() async throws {
        let outUrl = FileManager.default.temporaryDirectory
            .appendingPathComponent("sipsdk_test_\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: outUrl) }

        let recorder = AppleMediaRecorder()
        let rc = recorder.start(
            path: outUrl.path,
            sampleRate: 48000,
            channels: 2,
            videoWidth: 160,
            videoHeight: 120
        )
        XCTAssertEqual(rc, 0, "AppleMediaRecorder.start must return 0")

        let yPlane = [UInt8](repeating: 140, count: 160 * 120)
        let uPlane = [UInt8](repeating: 100, count: 80 * 60)
        let vPlane = [UInt8](repeating: 160, count: 80 * 60)

        var pcm = [Int16](repeating: 0, count: 960 * 2)
        for i in 0..<960 {
            let s = Int16(5000.0 * sin(2.0 * .pi * 440.0 * Double(i) / 48000.0))
            pcm[i * 2] = s
            pcm[i * 2 + 1] = s
        }

        for idx in 0..<6 {
            let audioTsNs = UInt64(idx) * 20_000_000
            pcm.withUnsafeBufferPointer { buf in
                recorder.writeAudio(pcm: buf.baseAddress!, sampleCount: buf.count, timestampNs: audioTsNs)
            }
            let videoTsNs = UInt64(idx) * 33_333_333
            yPlane.withUnsafeBufferPointer { yp in
                uPlane.withUnsafeBufferPointer { up in
                    vPlane.withUnsafeBufferPointer { vp in
                        let vf = sip_video_frame_t(
                            width: 160,
                            height: 120,
                            y: yp.baseAddress,
                            u: up.baseAddress,
                            v: vp.baseAddress,
                            y_stride: 160,
                            u_stride: 80,
                            v_stride: 80,
                            timestamp_ns: videoTsNs
                        )
                        recorder.writeVideo(frame: vf)
                    }
                }
            }
        }

        XCTAssertEqual(recorder.stop(), 0, "AppleMediaRecorder.stop must return 0")
        XCTAssertTrue(FileManager.default.fileExists(atPath: outUrl.path), "MP4 file must exist")

        let attrs = try FileManager.default.attributesOfItem(atPath: outUrl.path)
        let fileSize = (attrs[.size] as? NSNumber)?.intValue ?? 0
        XCTAssertGreaterThan(fileSize, 1024, "MP4 file must be non-empty")

        let asset = AVURLAsset(url: outUrl)
        let videoTracks = try await asset.loadTracks(withMediaType: .video)
        let audioTracks = try await asset.loadTracks(withMediaType: .audio)
        XCTAssertEqual(videoTracks.count, 1, "MP4 must contain 1 video track")
        XCTAssertEqual(audioTracks.count, 1, "MP4 must contain 1 audio track")
    }
}
