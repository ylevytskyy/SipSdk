//
//  SipCallsViewController.swift
//  TareSIPDemo
//
//  Created by Yuriy Levytskyy on 2/26/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

import UIKit

class SipCallsViewController: UIViewController {
    @IBOutlet weak var remoteUri: UITextField!
    @IBOutlet weak var callsTableView: UITableView!
    @IBOutlet weak var logsTextView: UITextView!
    
    private var client = SipClient(username: "", domain: "")
    private var calls: [SipCall] = []
    private var didReturnFromBackground = false
    private var wirelessDataAlertVisible = false
    
    @IBAction func connect(_ sender: Any) {
        guard let uri = remoteUri.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !uri.isEmpty else {
            log("Enter a SIP URI before starting a call.")
            return
        }

        guard let sipCall = client.makeCall(uri) else {
            log("Failed to create call. Check SIP registration and the destination URI.")
            return
        }

        calls.append(sipCall)
        callsTableView.reloadData()
    }

    @IBAction func register(_ sender: Any) {
        let result = client.registry()
        if result != 0 {
            log("Unable to register SIP client (code \(result): \(errorText(for: result))).")
        }
    }
    
    @IBAction func unregister(_ sender: Any) {
        client.unregister();
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        callsTableView.dataSource = self
        
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(applicationDidBecomeActive),
                                               name: UIApplication.didBecomeActiveNotification,
                                               object: nil)
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(applicationDidEnterBackground),
                                               name: UIApplication.didEnterBackgroundNotification,
                                               object: nil)
        startSipClient()
    }

    private struct SipCredentials {
        let username: String
        let domain: String
        let password: String
    }

    private static func sipCredentials() -> SipCredentials? {
        guard let url = Bundle.main.url(forResource: "SipCredentials", withExtension: "plist"),
              let dictionary = NSDictionary(contentsOf: url) as? [String: String],
              let username = dictionary["username"],
              let domain = dictionary["domain"],
              let password = dictionary["password"] else {
            return nil
        }
        return SipCredentials(username: username, domain: domain, password: password)
    }

    private func startSipClient() {
        guard let credentials = Self.sipCredentials() else {
            log("SipCredentials.plist is missing — copy TareSIPDemo/SipCredentials.example.plist to TareSIPDemo/SipCredentials.plist and fill in your SIP account.")
            return
        }
        client.stop()
        client = SipClient(username: credentials.username, domain: credentials.domain)
        client.delegate = self
        client.password = credentials.password
        let result = client.start()
        if result != 0 {
            log("Failed to start SIP client (code \(result): \(errorText(for: result))).")
        }
    }

    @objc private func applicationDidEnterBackground() {
        didReturnFromBackground = true
    }

    // Returning from Settings after enabling Wireless Data: recover without an app restart.
    @objc private func applicationDidBecomeActive() {
        guard didReturnFromBackground else { return }
        didReturnFromBackground = false

        // The SIP stack stays up even when start() reported failure; try re-registering
        // on the live client first, and fully restart the stack if that cannot run.
        if client.isRegistered() == 0 {
            log("Retrying SIP registration...")
            let result = client.registry()
            if result != 0 {
                log("Unable to register SIP client (code \(result): \(errorText(for: result))).")
                startSipClient()
            }
        }
        promptForWirelessDataIfNeeded()
    }

    private func errorText(for code: Int32) -> String {
        String(cString: strerror(code))
    }

    // Mirror on-screen log lines to NSLog so they are visible over devicectl console.
    private func log(_ message: String) {
        NSLog("%@", message)
        logsTextView.text += message + "\n"
    }
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        promptForWirelessDataIfNeeded()
    }

    // The system "Wireless Data" prompt appears only once; afterwards the app can
    // only detect the denial and deep-link into its own Settings page.
    private func promptForWirelessDataIfNeeded() {
        guard !wirelessDataAlertVisible else { return }
        DispatchQueue.global().async { [weak self] in
            guard self?.isWirelessDataRestricted() == true else { return }
            DispatchQueue.main.async {
                self?.presentWirelessDataAlert()
            }
        }
    }

    private func presentWirelessDataAlert() {
        guard !wirelessDataAlertVisible else { return }
        wirelessDataAlertVisible = true
        NSLog("Wireless data route is blocked; presenting settings prompt")

        let dismiss: (UIAlertAction) -> Void = { [weak self] _ in
            self?.wirelessDataAlertVisible = false
        }
        let alert = UIAlertController(
            title: "Wireless Data Off",
            message: "Wireless data is disabled for this app, so SIP registration cannot reach the server. Enable WLAN & Cellular data for TareSIPDemo in Settings.",
            preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Open Settings", style: .default) { action in
            dismiss(action)
            if let url = URL(string: UIApplication.openSettingsURLString) {
                UIApplication.shared.open(url)
            }
        })
        alert.addAction(UIAlertAction(title: "Ignore", style: .cancel, handler: dismiss))
        present(alert, animated: true)
    }

    private func isWirelessDataRestricted() -> Bool {
        // A denied app cannot route packets; a UDP send fails with EHOSTUNREACH or EPERM.
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = in_port_t(5060).bigEndian
        inet_pton(AF_INET, "8.8.8.8", &address.sin_addr)

        let socketDescriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socketDescriptor >= 0 else { return false }
        defer { close(socketDescriptor) }

        var probe: UInt8 = 0
        let sent = withUnsafePointer(to: &address) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddr in
                sendto(socketDescriptor, &probe, 1, 0, sockaddr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard sent < 0 else { return false }
        let blockedErrors = [EHOSTUNREACH, EPERM, ENETUNREACH, EADDRNOTAVAIL]
        return blockedErrors.contains(errno)
    }

    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        if segue.identifier == "sipCallDetailsSegue" {
            let path = callsTableView.indexPathForSelectedRow!
            let destination = segue.destination as! SipCallDetailsViewController
            destination.sipCall = calls[path.row]
        }
    }
}

extension SipCallsViewController : SipClientDelegate {
    func onWillRegister(_ sipSdk: SipClient) {
        log("Registering...")
    }
    
    func onDidRegister(_ sipSdk: SipClient) {
        log("Registered")
    }
    
    func onFailedRegister(_ sipSdk: SipClient) {
        log("Failed to register")
        promptForWirelessDataIfNeeded()
    }
    
    func onWillUnRegister(_ sipSdk: SipClient) {
        log("Unregistering...")
    }
    
    func onCallIncoming(_ sipCall: SipCall) {
        log("Incomming call from \(sipCall.remoteUri)...")
        
        let alertController = UIAlertController(title: sipCall.remoteUri, message: "Incoming Call", preferredStyle: .alert)
        alertController.addAction(UIAlertAction(title: "Answer", style: .default) { _ in
            sipCall.answer()
        })
        alertController.addAction(UIAlertAction(title: "Decline", style: .default) { _ in
            sipCall.hangup(486, reason: "Busy Here")
        })
        
        self.present(alertController, animated: true, completion: nil)
        
        if !calls.contains(where: { $0 === sipCall }) {
            calls.append(sipCall)
        }
        callsTableView.reloadData()
    }
    
    func onCallRinging(_ sipCall: SipCall) {
        log("Call ringing from \(sipCall.remoteUri)...")
    }
    
    func onCallProcess(_ sipCall: SipCall) {
        log("Call process from \(sipCall.remoteUri)...")
    }
    
    func onCallEstablished(_ sipCall: SipCall) {
        log("Call established from \(sipCall.remoteUri)...")
    }
    
    func onCallClosed(_ sipCall: SipCall) {
        log("Call closed from \(sipCall.remoteUri)...")
        
        calls.removeAll(where: { $0 === sipCall })
        callsTableView.reloadData()
    }
}

extension SipCallsViewController : UITableViewDataSource {
    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return calls.count
    }
    
    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "CellId", for: indexPath) as! CallCell
        let sipCall = calls[indexPath.row]
        cell.remoteUriLabel.text = sipCall.remoteUri
        return cell
    }
}
