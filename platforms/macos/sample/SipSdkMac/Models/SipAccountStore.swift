import Foundation

public final class SipAccountStore: ObservableObject {
    private enum Keys {
        static let username = "sipsdk.mac.account.username"
        static let domain = "sipsdk.mac.account.domain"
        static let password = "sipsdk.mac.account.password"
        static let port = "sipsdk.mac.account.port"
        static let transport = "sipsdk.mac.account.transport"
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

    public init() {
        let defaults = UserDefaults.standard
        self.username = defaults.string(forKey: Keys.username) ?? "1001"
        self.domain = defaults.string(forKey: Keys.domain) ?? "127.0.0.1"
        self.password = defaults.string(forKey: Keys.password) ?? "test1001"
        let savedPort = defaults.integer(forKey: Keys.port)
        self.port = savedPort > 0 ? savedPort : 5060
        self.transport = defaults.string(forKey: Keys.transport) ?? "udp"
    }

    public func resetToDefaults() {
        self.username = "1001"
        self.domain = "127.0.0.1"
        self.password = "test1001"
        self.port = 5060
        self.transport = "udp"
    }
}
