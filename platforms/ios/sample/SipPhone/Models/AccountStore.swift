//
//  AccountStore.swift
//  SipPhone (iOS Sample App)
//

import Foundation
import SipSdk

@Observable
public final class AccountStore {
    private let userDefaults: UserDefaults
    private let accountKey = "com.sipsdk.sample.account"

    public var account: SipAccount {
        didSet {
            save()
        }
    }

    public init(userDefaults: UserDefaults = .standard) {
        self.userDefaults = userDefaults

        #if targetEnvironment(simulator)
        let defaultHost = "127.0.0.1"
        #else
        let defaultHost = "192.168.0.161"
        #endif

        if let data = userDefaults.data(forKey: accountKey),
           var saved = try? JSONDecoder().decode(SipAccount.self, from: data) {
            #if !targetEnvironment(simulator)
            if saved.domain == "127.0.0.1" {
                saved.domain = defaultHost
            }
            #endif
            self.account = saved
        } else {
            // Default to local Asterisk PBX
            self.account = SipAccount(
                username: "1001",
                domain: defaultHost,
                password: "test1001",
                transport: .udp,
                port: 5060,
                displayName: "iPhone 1001",
                outboundProxy: nil,
                stunServer: nil,
                mediaNat: nil,
                regInterval: 3600
            )
        }
    }

    public func save() {
        if let data = try? JSONEncoder().encode(account) {
            userDefaults.set(data, forKey: accountKey)
        }
    }

    public func resetToDefaults() {
        #if targetEnvironment(simulator)
        let defaultHost = "127.0.0.1"
        #else
        let defaultHost = "192.168.0.161"
        #endif

        self.account = SipAccount(
            username: "1001",
            domain: defaultHost,
            password: "test1001",
            transport: .udp,
            port: 5060,
            displayName: "iPhone 1001"
        )
    }
}
