import Foundation

public final class SipAccountStore: ObservableObject {
    private enum Keys {
        static let username = "sipsdk.mac.account.username"
        static let domain = "sipsdk.mac.account.domain"
        static let password = "sipsdk.mac.account.password"
        static let port = "sipsdk.mac.account.port"
        static let transport = "sipsdk.mac.account.transport"
        static let displayName = "sipsdk.mac.account.display_name"
        static let outboundProxy = "sipsdk.mac.account.outbound_proxy"
        static let stunServer = "sipsdk.mac.account.stun_server"
        static let useIce = "sipsdk.mac.account.use_ice"
        static let regInterval = "sipsdk.mac.account.reg_interval"
    }

    @Published public var username: String {
        didSet { UserDefaults.standard.set(username, forKey: Keys.username) }
    }
    @Published public var domain: String {
        didSet { UserDefaults.standard.set(domain, forKey: Keys.domain) }
    }
    @Published public var password: String {
        didSet { UserDefaults.standard.set(password, forKey: Keys.password) }
    }
    @Published public var port: Int {
        didSet { UserDefaults.standard.set(port, forKey: Keys.port) }
    }
    @Published public var transport: String {
        didSet { UserDefaults.standard.set(transport, forKey: Keys.transport) }
    }
    @Published public var displayName: String {
        didSet { UserDefaults.standard.set(displayName, forKey: Keys.displayName) }
    }
    @Published public var outboundProxy: String {
        didSet { UserDefaults.standard.set(outboundProxy, forKey: Keys.outboundProxy) }
    }
    @Published public var stunServer: String {
        didSet { UserDefaults.standard.set(stunServer, forKey: Keys.stunServer) }
    }
    @Published public var useIce: Bool {
        didSet { UserDefaults.standard.set(useIce, forKey: Keys.useIce) }
    }
    @Published public var regInterval: UInt32 {
        didSet { UserDefaults.standard.set(regInterval, forKey: Keys.regInterval) }
    }

    public init() {
        let defaults = UserDefaults.standard
        self.username = defaults.string(forKey: Keys.username) ?? "1001"
        self.domain = defaults.string(forKey: Keys.domain) ?? "127.0.0.1"
        self.password = defaults.string(forKey: Keys.password) ?? "test1001"
        let savedPort = defaults.integer(forKey: Keys.port)
        self.port = savedPort > 0 ? savedPort : 5060
        self.transport = defaults.string(forKey: Keys.transport) ?? "udp"
        self.displayName = defaults.string(forKey: Keys.displayName) ?? ""
        self.outboundProxy = defaults.string(forKey: Keys.outboundProxy) ?? ""
        self.stunServer = defaults.string(forKey: Keys.stunServer) ?? ""
        self.useIce = defaults.bool(forKey: Keys.useIce)
        let savedInterval = defaults.integer(forKey: Keys.regInterval)
        self.regInterval = savedInterval > 0 ? UInt32(savedInterval) : 3600
    }

    public func resetToDefaults() {
        self.username = "1001"
        self.domain = "127.0.0.1"
        self.password = "test1001"
        self.port = 5060
        self.transport = "udp"
        self.displayName = ""
        self.outboundProxy = ""
        self.stunServer = ""
        self.useIce = false
        self.regInterval = 3600
    }
}
