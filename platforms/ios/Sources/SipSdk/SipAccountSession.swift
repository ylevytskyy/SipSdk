//
//  SipAccountSession.swift
//  SipSdk (iOS Platform)
//

import Foundation
import SipCore

/// Represents an active SIP account registered on a `SipClient` (multi-account support).
@MainActor
@Observable
public final class SipAccountSession: Identifiable {
    /// Stable monotonic identifier assigned by the core engine.
    public let id: UInt64

    /// Account configuration profile used to create this session.
    public let profile: SipAccount

    /// Canonical Address of Record (AOR) string.
    public let aor: String

    /// Current registration state for this account.
    public private(set) var registrationState: SipRegistrationState = .registering

    /// True if this account is currently registered.
    public var isRegistered: Bool {
        if let handle = coreAccount {
            return sip_account_is_registered(handle)
        }
        return false
    }

    @ObservationIgnored nonisolated(unsafe) internal var coreAccount: OpaquePointer?

    internal init(coreAccount: OpaquePointer, profile: SipAccount) {
        self.coreAccount = coreAccount
        self.profile = profile
        self.id = sip_account_get_id(coreAccount)
        if let aorPtr = sip_account_get_aor(coreAccount) {
            self.aor = String(cString: aorPtr)
        } else {
            self.aor = profile.aor
        }
    }

    /// Triggers re-registration for this account.
    public func reregister() throws {
        guard let handle = coreAccount else {
            throw SipError.invalidParameter("Account session handle is invalid")
        }
        let rc = sip_account_reregister(handle)
        if rc != 0 {
            throw SipError.coreError(Int(rc), "Failed to reregister account")
        }
    }

    /// Unregisters this account without removing it from the client.
    public func unregister() throws {
        guard let handle = coreAccount else {
            throw SipError.invalidParameter("Account session handle is invalid")
        }
        let rc = sip_account_unregister(handle)
        if rc != 0 {
            throw SipError.coreError(Int(rc), "Failed to unregister account")
        }
        registrationState = .unregistered
    }

    internal func updateRegistrationState(_ newState: SipRegistrationState) {
        self.registrationState = newState
    }
}
