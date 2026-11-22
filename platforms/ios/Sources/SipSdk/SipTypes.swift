//
//  SipTypes.swift
//  SipSdk (iOS Platform)
//

import Foundation

/// SIP registration states.
public enum SipRegistrationState: Equatable, Sendable, CustomStringConvertible {
    case unregistered
    case registering
    case registered
    case failed(code: Int, reason: String)

    public var isRegistered: Bool {
        if case .registered = self { return true }
        return false
    }

    public var description: String {
        switch self {
        case .unregistered: return "Unregistered"
        case .registering: return "Registering"
        case .registered: return "Registered"
        case .failed(let code, let reason): return "Failed (\(code): \(reason))"
        }
    }
}

/// Call session lifecycle states.
public enum SipCallState: Equatable, Sendable, CustomStringConvertible {
    case incoming
    case ringing
    case connecting
    case established
    case hold
    case closed(code: Int, reason: String)

    public var isClosed: Bool {
        if case .closed = self { return true }
        return false
    }

    public var isConnected: Bool {
        switch self {
        case .established, .hold: return true
        default: return false
        }
    }

    public var description: String {
        switch self {
        case .incoming: return "Incoming"
        case .ringing: return "Ringing"
        case .connecting: return "Connecting"
        case .established: return "Established"
        case .hold: return "On Hold"
        case .closed(let code, let reason): return "Closed (\(code): \(reason))"
        }
    }
}

/// Camera hardware selection.
public enum SipCameraPosition: Int, Sendable, Equatable, Hashable {
    case front = 0
    case back = 1
}

/// Network transport protocol.
public enum SipTransport: String, Sendable, Codable, CaseIterable {
    case udp = "udp"
    case tcp = "tcp"
    case tls = "tls"
    case ws = "ws"
    case wss = "wss"
}

/// SIP account configuration profile.
public struct SipAccount: Sendable, Codable, Hashable {
    public var username: String
    public var domain: String
    public var password: String
    public var transport: SipTransport
    public var port: Int
    public var displayName: String?
    public var outboundProxy: String?
    public var stunServer: String?
    public var stunUser: String?
    public var stunPassword: String?
    public var mediaNat: String?
    public var regInterval: UInt32
    public var tlsCaFile: String?
    public var verifyTlsCert: Bool
    public var mediaEnc: String?
    public var maxCalls: Int

    public init(
        username: String,
        domain: String,
        password: String = "",
        transport: SipTransport = .udp,
        port: Int = 5060,
        displayName: String? = nil,
        outboundProxy: String? = nil,
        stunServer: String? = nil,
        stunUser: String? = nil,
        stunPassword: String? = nil,
        mediaNat: String? = nil,
        regInterval: UInt32 = 0,
        tlsCaFile: String? = nil,
        verifyTlsCert: Bool = true,
        mediaEnc: String? = nil,
        maxCalls: Int = 4
    ) {
        self.username = username
        self.domain = domain
        self.password = password
        self.transport = transport
        self.port = port
        self.displayName = displayName
        self.outboundProxy = outboundProxy
        self.stunServer = stunServer
        self.stunUser = stunUser
        self.stunPassword = stunPassword
        self.mediaNat = mediaNat
        self.regInterval = regInterval
        self.tlsCaFile = tlsCaFile
        self.verifyTlsCert = verifyTlsCert
        self.mediaEnc = mediaEnc
        self.maxCalls = max(1, min(8, maxCalls))
    }

    /// Canonical Address of Record (AOR).
    public var aor: String {
        let tp: String
        switch transport {
        case .tcp: tp = ";transport=tcp"
        case .tls: tp = ";transport=tls"
        case .ws: tp = ";transport=ws"
        case .wss: tp = ";transport=wss"
        case .udp: tp = ""
        }
        return "sip:\(username)@\(domain):\(port)\(tp)"
    }
}

/// Real-time call quality telemetry metrics.
public struct SipCallStats: Sendable, Equatable {
    public let audioCodec: String
    public let audioBitrateKbps: UInt32
    public let audioLossRate: Float
    public let audioJitterMs: UInt32
    public let audioRttMs: UInt32
    public let videoCodec: String
    public let videoBitrateKbps: UInt32
    public let videoLossRate: Float
    public let videoJitterMs: UInt32
    public let videoFps: UInt32

    public init(
        audioCodec: String = "",
        audioBitrateKbps: UInt32 = 0,
        audioLossRate: Float = 0.0,
        audioJitterMs: UInt32 = 0,
        audioRttMs: UInt32 = 0,
        videoCodec: String = "",
        videoBitrateKbps: UInt32 = 0,
        videoLossRate: Float = 0.0,
        videoJitterMs: UInt32 = 0,
        videoFps: UInt32 = 0
    ) {
        self.audioCodec = audioCodec
        self.audioBitrateKbps = audioBitrateKbps
        self.audioLossRate = audioLossRate
        self.audioJitterMs = audioJitterMs
        self.audioRttMs = audioRttMs
        self.videoCodec = videoCodec
        self.videoBitrateKbps = videoBitrateKbps
        self.videoLossRate = videoLossRate
        self.videoJitterMs = videoJitterMs
        self.videoFps = videoFps
    }
}

/// Status of a message sent with `SipClient.sendMessage`.
public enum SipMessageStatus: Int, Sendable, Equatable {
    /// The server or peer accepted the message (2xx).
    case sent = 0
    /// Non-2xx final response, transport error, or no registered account.
    case failed = 1
    /// Reserved: delivery notification (IMDN, RFC 5438) from the peer.
    case delivered = 2
    /// Reserved: display (read) notification (IMDN, RFC 5438) from the peer.
    case displayed = 3
}

/// A SIP MESSAGE received for the registered account.
public struct SipIncomingMessage: Sendable, Hashable {
    /// Sender address, e.g. `sip:1002@pbx.example.com`.
    public let from: String
    /// MIME type, e.g. `text/plain`.
    public let contentType: String
    /// Raw body bytes.
    public let body: Data
    public let receivedAt: Date

    public init(from: String, contentType: String, body: Data, receivedAt: Date = Date()) {
        self.from = from
        self.contentType = contentType
        self.body = body
        self.receivedAt = receivedAt
    }

    /// Body as UTF-8 text for `text/*` content types; nil otherwise.
    public var text: String? {
        guard contentType.lowercased().hasPrefix("text/") else { return nil }
        return String(data: body, encoding: .utf8)
    }
}

/// Recording format and channel layout for local call recording.
public enum SipRecordingMode: Int, Sendable, Equatable {
    /// 48 kHz S16LE stereo WAV (left = local mic, right = remote peer).
    case audioStereo = 0
    /// 48 kHz S16LE mono WAV (local + remote mixed).
    case audioMono = 1
    /// Audio + remote video MP4 (via AVAssetWriter).
    case audioVideo = 2
}

/// Recording lifecycle state reported for a call.
public enum SipRecordingState: Int, Sendable, Equatable {
    case idle = 0
    case recording = 1
    case failed = 2
}

/// Typed event dispatch streamed by `SipClient`.
public enum SipEvent: Sendable {
    case registrationChanged(SipRegistrationState)
    case incomingCall(SipCall)
    case callStateChanged(SipCall, SipCallState)
    case mediaUpdated(SipCall, width: Int, height: Int, onHold: Bool, isMuted: Bool)
    case dtmfReceived(SipCall, digit: String)
    case callStatsUpdated(SipCall, SipCallStats)
    case transferStatusReceived(SipCall, code: Int, reason: String)
    case transferRequested(SipCall, referToUri: String, hasReplaces: Bool)
    case messageReceived(SipIncomingMessage)
    case messageStatusChanged(id: UInt64, status: SipMessageStatus, code: Int, reason: String)
    case callRejectedBusy(String)
    case conferenceChanged([SipCall])
    case recordingStateChanged(SipCall, SipRecordingState, path: String, errorCode: Int)
}

/// Strongly typed SIP error domain.
public enum SipError: LocalizedError, Sendable {
    case engineNotStarted
    case alreadyStarted
    case registrationFailed(Int, String)
    case callCreationFailed(String)
    case busy
    case messageSendFailed(Int, String)
    case invalidParameter(String)
    case audioSessionFailed(String)
    case coreError(Int, String)

    public var errorDescription: String? {
        switch self {
        case .engineNotStarted:
            return "SIP core engine is not running"
        case .alreadyStarted:
            return "SIP core engine is already running"
        case .registrationFailed(let code, let reason):
            return "SIP registration failed (\(code)): \(reason)"
        case .callCreationFailed(let detail):
            return "Failed to create SIP call: \(detail)"
        case .busy:
            return "Maximum concurrent calls reached (Busy)"
        case .messageSendFailed(let code, let detail):
            return "Failed to send SIP message (\(code)): \(detail)"
        case .invalidParameter(let detail):
            return "Invalid parameter: \(detail)"
        case .audioSessionFailed(let detail):
            return "Audio hardware session error: \(detail)"
        case .coreError(let code, let msg):
            return "Core SIP stack error (\(code)): \(msg)"
        }
    }
}
