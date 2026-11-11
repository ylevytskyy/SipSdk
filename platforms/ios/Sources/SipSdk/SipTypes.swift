//
//  SipTypes.swift
//  SipSdk (iOS Platform)
//

import Foundation
import SipCore

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
        regInterval: UInt32 = 0
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
    }

    /// Canonical Address of Record (AOR).
    public var aor: String {
        let tp = transport == .tcp ? ";transport=tcp" : ""
        return "sip:\(username)@\(domain):\(port)\(tp)"
    }

    /// Scopes all required and optional C strings for the duration of `body` so no pointer escapes prematurely.
    internal func withCAccountConfig<R>(_ body: (inout sip_account_config_t) -> R) -> R {
        func withOptionalCString<T>(_ string: String?, _ closure: (UnsafePointer<CChar>?) -> T) -> T {
            guard let string else { return closure(nil) }
            return string.withCString { closure($0) }
        }

        return username.withCString { userPtr in
            domain.withCString { domainPtr in
                password.withCString { pwdPtr in
                    transport.rawValue.withCString { tpPtr in
                        withOptionalCString(displayName) { dnamePtr in
                            withOptionalCString(outboundProxy) { obPtr in
                                withOptionalCString(stunServer) { stunPtr in
                                    withOptionalCString(stunUser) { stunUserPtr in
                                        withOptionalCString(stunPassword) { stunPassPtr in
                                            withOptionalCString(mediaNat) { natPtr in
                                                var cConfig = sip_account_config_t()
                                                cConfig.user = userPtr
                                                cConfig.domain = domainPtr
                                                cConfig.pwd = pwdPtr
                                                cConfig.transport = tpPtr
                                                cConfig.port = Int32(port)
                                                cConfig.reg_interval = regInterval
                                                cConfig.display_name = dnamePtr
                                                cConfig.outbound_proxy = obPtr
                                                cConfig.stun_server = stunPtr
                                                cConfig.stun_user = stunUserPtr
                                                cConfig.stun_pass = stunPassPtr
                                                cConfig.media_nat = natPtr
                                                return body(&cConfig)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/// Typed event dispatch streamed by `SipClient`.
public enum SipEvent: Sendable {
    case registrationChanged(SipRegistrationState)
    case incomingCall(SipCall)
    case callStateChanged(SipCall, SipCallState)
    case mediaUpdated(SipCall, width: Int, height: Int, onHold: Bool, isMuted: Bool)
    case dtmfReceived(SipCall, digit: String)
}

/// Strongly typed SIP error domain.
public enum SipError: LocalizedError, Sendable {
    case engineNotStarted
    case alreadyStarted
    case registrationFailed(Int, String)
    case callCreationFailed(String)
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
        case .invalidParameter(let detail):
            return "Invalid parameter: \(detail)"
        case .audioSessionFailed(let detail):
            return "Audio hardware session error: \(detail)"
        case .coreError(let code, let msg):
            return "Core SIP stack error (\(code)): \(msg)"
        }
    }
}
