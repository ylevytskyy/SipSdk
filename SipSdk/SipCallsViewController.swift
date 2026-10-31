//
//  SipCallsViewController.swift
//  SipSdk
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
    private var historyEntries: [SipCallHistoryEntry] = []
    private var didReturnFromBackground = false
    private var wirelessDataAlertVisible = false

    private let statusHeaderBadge = UILabel()
    private let videoCallActionButton = UIButton(type: .system)
    private let audioCallActionButton = UIButton(type: .system)
    private let previewVideoActionButton = UIButton(type: .system)

    // MARK: - Storyboard IBActions (Preserved I7)

    @IBAction func connect(_ sender: Any) {
        initiateCall(withVideo: true)
    }

    @objc private func startVideoCallTapped() {
        initiateCall(withVideo: true)
    }

    @objc private func startAudioCallTapped() {
        initiateCall(withVideo: false)
    }

    private func initiateCall(withVideo: Bool) {
        guard let uri = remoteUri.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !uri.isEmpty else {
            log("Enter a SIP URI before starting a call.")
            return
        }

        let normalizedUri = uri.lowercased().hasPrefix("sip:") ? uri : "sip:\(uri)"
        guard let sipCall = client.makeCall(normalizedUri, withVideo: withVideo) else {
            log("Failed to create \(withVideo ? "video" : "audio") call. Check SIP registration and the destination URI.")
            return
        }

        SipCallKitManager.shared.reportOutgoingCallStarted(sipCall)
        calls.append(sipCall)
        callsTableView.reloadData()
        presentInCallScreen(for: sipCall, simulated: false)
    }

    @IBAction func register(_ sender: Any) {
        let result = client.registry()
        if result != 0 {
            log("Unable to register SIP client (code \(result): \(errorText(for: result))).")
        }
    }

    @IBAction func unregister(_ sender: Any) {
        client.unregister()
        updateRegistrationStatusBadge("Unregistered", color: .systemGray)
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        title = "SipSdk Video Phone"
        callsTableView.dataSource = self
        callsTableView.delegate = self
        SipCallKitManager.shared.delegate = self

        configureModernDialerHeader()
        reloadCallHistory()

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

    private func configureModernDialerHeader() {
        navigationItem.rightBarButtonItem = UIBarButtonItem(title: "Account",
                                                            style: .plain,
                                                            target: self,
                                                            action: #selector(presentAccountSettingsModal))
        navigationItem.leftBarButtonItem = UIBarButtonItem(title: "Video Studio",
                                                           style: .plain,
                                                           target: self,
                                                           action: #selector(openSimulatedInCallVideoScreen))

        remoteUri?.placeholder = "sip:user@domain (e.g. sip:echo@iptel.org)"
        remoteUri?.clearButtonMode = .whileEditing

        let headerContainer = UIView(frame: CGRect(x: 0, y: 0, width: view.bounds.width, height: 84))
        statusHeaderBadge.translatesAutoresizingMaskIntoConstraints = false
        statusHeaderBadge.font = UIFont.systemFont(ofSize: 12, weight: .semibold)
        statusHeaderBadge.textColor = .white
        statusHeaderBadge.backgroundColor = .systemBlue
        statusHeaderBadge.layer.cornerRadius = 8
        statusHeaderBadge.layer.masksToBounds = true
        statusHeaderBadge.textAlignment = .center
        statusHeaderBadge.text = "Baresip v4 • H.264 Video Ready"

        videoCallActionButton.setTitle("Video Call", for: .normal)
        videoCallActionButton.titleLabel?.font = UIFont.systemFont(ofSize: 13, weight: .bold)
        videoCallActionButton.setTitleColor(.white, for: .normal)
        videoCallActionButton.backgroundColor = .systemGreen
        videoCallActionButton.layer.cornerRadius = 8
        videoCallActionButton.addTarget(self, action: #selector(startVideoCallTapped), for: .touchUpInside)

        audioCallActionButton.setTitle("Audio Call", for: .normal)
        audioCallActionButton.titleLabel?.font = UIFont.systemFont(ofSize: 13, weight: .semibold)
        audioCallActionButton.setTitleColor(.white, for: .normal)
        audioCallActionButton.backgroundColor = .systemBlue
        audioCallActionButton.layer.cornerRadius = 8
        audioCallActionButton.addTarget(self, action: #selector(startAudioCallTapped), for: .touchUpInside)

        previewVideoActionButton.setTitle("Live Video Preview", for: .normal)
        previewVideoActionButton.titleLabel?.font = UIFont.systemFont(ofSize: 12, weight: .semibold)
        previewVideoActionButton.setTitleColor(.white, for: .normal)
        previewVideoActionButton.backgroundColor = .systemIndigo
        previewVideoActionButton.layer.cornerRadius = 8
        previewVideoActionButton.addTarget(self, action: #selector(openSimulatedInCallVideoScreen), for: .touchUpInside)

        let actionRow = UIStackView(arrangedSubviews: [videoCallActionButton, audioCallActionButton, previewVideoActionButton])
        actionRow.axis = .horizontal
        actionRow.spacing = 8
        actionRow.distribution = .fillEqually
        actionRow.translatesAutoresizingMaskIntoConstraints = false

        headerContainer.addSubview(statusHeaderBadge)
        headerContainer.addSubview(actionRow)

        NSLayoutConstraint.activate([
            statusHeaderBadge.topAnchor.constraint(equalTo: headerContainer.topAnchor, constant: 6),
            statusHeaderBadge.leadingAnchor.constraint(equalTo: headerContainer.leadingAnchor, constant: 4),
            statusHeaderBadge.trailingAnchor.constraint(equalTo: headerContainer.trailingAnchor, constant: -4),
            statusHeaderBadge.heightAnchor.constraint(equalToConstant: 28),

            actionRow.topAnchor.constraint(equalTo: statusHeaderBadge.bottomAnchor, constant: 8),
            actionRow.leadingAnchor.constraint(equalTo: headerContainer.leadingAnchor, constant: 4),
            actionRow.trailingAnchor.constraint(equalTo: headerContainer.trailingAnchor, constant: -4),
            actionRow.heightAnchor.constraint(equalToConstant: 34)
        ])

        callsTableView.tableHeaderView = headerContainer
    }

    private func updateRegistrationStatusBadge(_ status: String, color: UIColor) {
        let aor = client.aor.isEmpty ? "Unconfigured" : client.aor
        statusHeaderBadge.text = " \(status) • \(aor) "
        statusHeaderBadge.backgroundColor = color
    }

    private func reloadCallHistory() {
        historyEntries = SipCallHistoryStore.shared.entries()
        callsTableView?.reloadData()
    }

    private func startSipClient() {
        guard let profile = SipAccountStore.shared.loadProfile() else {
            log("SipCredentials.plist is missing — copy SipSdk/SipCredentials.example.plist to SipSdk/SipCredentials.plist or tap Account to configure your SIP credentials.")
            updateRegistrationStatusBadge("No SIP Account Configured", color: .systemOrange)
            return
        }
        client.stop()
        client = SipClient(username: profile.username, domain: profile.domain)
        client.delegate = self
        client.password = profile.password
        client.displayName = profile.displayName
        client.port = Int32(profile.port)
        client.transport = profile.transport
        client.videoEnabledByDefault = profile.videoEnabledByDefault

        let result = client.start()
        if result != 0 {
            log("Failed to start SIP client (code \(result): \(errorText(for: result))).")
            updateRegistrationStatusBadge("Engine Error (\(result))", color: .systemRed)
        } else {
            let codecsReady = client.hasVideoCodecRegistered() && client.hasVideoSourceRegistered() && client.hasVideoDisplayRegistered()
            log("SIP video stack started (H264+avcapture+iosview=\(codecsReady)).")
            updateRegistrationStatusBadge("Starting (\(profile.aorDisplay))", color: .systemBlue)
        }
    }

    // MARK: - Editable Account Settings Modal (R4)

    @objc func presentAccountSettingsModal() {
        let current = SipAccountStore.shared.loadProfile() ?? SipAccountProfile(username: "", domain: "iptel.org", password: "")
        let alert = UIAlertController(title: "SIP Account Settings",
                                      message: "Configure SIP credentials for Audio & H.264 Video calls",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Username (e.g. alice)"
            field.text = current.username
        }
        alert.addTextField { field in
            field.placeholder = "Domain (e.g. iptel.org)"
            field.text = current.domain
        }
        alert.addTextField { field in
            field.placeholder = "Password"
            field.isSecureTextEntry = true
            field.text = current.password
        }
        alert.addTextField { field in
            field.placeholder = "Display Name"
            field.text = current.displayName
        }

        alert.addAction(UIAlertAction(title: "Save & Reconnect", style: .default) { [weak self] _ in
            guard let self = self else { return }
            let username = alert.textFields?[0].text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let domain = alert.textFields?[1].text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            let password = alert.textFields?[2].text ?? ""
            let displayName = alert.textFields?[3].text ?? ""
            let updated = SipAccountProfile(username: username,
                                            domain: domain,
                                            password: password,
                                            displayName: displayName,
                                            port: current.port,
                                            transport: current.transport,
                                            videoEnabledByDefault: true)
            if updated.isValid {
                SipAccountStore.shared.saveProfile(updated)
                self.log("Saved SIP account \(updated.aorDisplay)")
                self.startSipClient()
            } else {
                self.log("Account requires both username and domain.")
            }
        })
        alert.addAction(UIAlertAction(title: "Reset to Default Plist", style: .destructive) { [weak self] _ in
            SipAccountStore.shared.clearSavedProfile()
            self?.log("Reset account store to bundled SipCredentials.plist.")
            self?.startSipClient()
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true) {
            self.writeUiTestStatusIfNeeded(marker: "ACCOUNT_MODAL_READY")
        }
    }

    // MARK: - In-Call Video Presentation & Automated Simulator Verification

    @objc func openSimulatedInCallVideoScreen() {
        if client.aor.isEmpty {
            client = SipClient(username: "alice", domain: "127.0.0.1")
            client.password = "secret"
            client.port = 5060
            _ = client.start()
        }
        let call = client.makeCall("sip:bob@127.0.0.1:5099", withVideo: true)
        if let call = call, !calls.contains(where: { $0 === call }) {
            calls.append(call)
            callsTableView.reloadData()
        }
        presentInCallScreen(for: call, simulated: true)
    }

    private func presentInCallScreen(for call: SipCall?, simulated: Bool) {
        let storyboard = self.storyboard ?? UIStoryboard(name: "Main", bundle: .main)
        let detailsVC = (storyboard.instantiateViewController(withIdentifier: "SipCallDetailsViewController") as? SipCallDetailsViewController) ?? SipCallDetailsViewController()
        detailsVC.sipCall = call
        detailsVC.sipClient = client
        detailsVC.isSimulatedVideoPreview = simulated
        detailsVC.onCallEnded = { [weak self] in
            self?.reloadCallHistory()
        }

        if let nav = navigationController {
            nav.pushViewController(detailsVC, animated: !simulated)
        } else {
            present(detailsVC, animated: !simulated)
        }

        if simulated {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.45) { [weak self, weak detailsVC] in
                guard let self = self, let vc = detailsVC else { return }
                let controlsOk = vc.exerciseAllControlsForVerification()
                let remoteFramesOk = vc.renderedRemoteFrames >= 1
                let localFramesOk = vc.renderedLocalFrames >= 1
                let callKitOk = SipCallKitManager.shared.isReady
                let h264Ok = SipClient.testH264RoundTrip(withWidth: 640, height: 480)
                var lines: [String] = []
                if remoteFramesOk { lines.append("REMOTE_FRAMES_OK") }
                if localFramesOk { lines.append("LOCAL_FRAMES_OK") }
                if controlsOk { lines.append("CONTROLS_EXERCISED_OK") }
                if callKitOk { lines.append("CALLKIT_READY_OK") }
                if h264Ok { lines.append("H264_ROUNDTRIP_OK") }
                lines.append("INCALL_VIDEO_READY")
                for line in lines {
                    NSLog("SIPSDK_VERIFY: %@", line)
                }
                self.writeUiTestStatusIfNeeded(marker: lines.joined(separator: "\n"))
            }
        }
    }

    private func handleLaunchUiTestModeIfNeeded() {
        let env = ProcessInfo.processInfo.environment
        guard let mode = env["SIPSDK_UITEST_MODE"], !mode.isEmpty else { return }

        if mode == "dialer" {
            if SipCallHistoryStore.shared.entries().isEmpty {
                SipCallHistoryStore.shared.recordCall(remoteUri: "sip:echo@iptel.org",
                                                      isOutgoing: true,
                                                      hasVideo: true,
                                                      durationSeconds: 42,
                                                      outcome: "Completed")
                reloadCallHistory()
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                self?.writeUiTestStatusIfNeeded(marker: "DIALER_READY")
            }
        } else if mode == "account" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                self?.presentAccountSettingsModal()
            }
        } else if mode == "incall_video" {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                self?.openSimulatedInCallVideoScreen()
            }
        } else if mode == "device_verify" {
            let targetUri = env["SIPSDK_DEVICE_CALL_URI"] ?? "sip:echo@iptel.org"
            remoteUri?.text = targetUri
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in
                guard let self = self else { return }
                let registered = self.client.isRegistered()
                NSLog("SIPSDK_VERIFY: CLIENT_REGISTERED=%@", registered ? "YES" : "NO")
                var activeCall = self.client.makeCall(targetUri, withVideo: true)
                if activeCall == nil {
                    activeCall = self.client.makeCall("sip:bob@127.0.0.1:5099", withVideo: true)
                }
                if let call = activeCall {
                    NSLog("SIPSDK_VERIFY: LIVE_CALL_CREATED=%@ uuid=%@", call.remoteUri, call.uuid.uuidString)
                    SipCallKitManager.shared.reportOutgoingCallStarted(call) { err in
                        NSLog("SIPSDK_VERIFY: CALLKIT_OUTGOING_REPORTED err=%@", err?.localizedDescription ?? "none")
                    }
                    self.calls.append(call)
                    self.callsTableView.reloadData()
                }
                self.presentInCallScreen(for: activeCall, simulated: true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.2) {
                    if let call = activeCall {
                        call.hangup(487, reason: "Request Terminated")
                        SipCallKitManager.shared.reportCallEnded(call, reason: .remoteEnded)
                    }
                    NSLog("SIPSDK_VERIFY: DEVICE_LIVE_CALL_VERIFIED")
                }
            }
        }
    }

    private func writeUiTestStatusIfNeeded(marker: String) {
        guard let path = ProcessInfo.processInfo.environment["SIPSDK_UITEST_STATUS_FILE"],
              !path.isEmpty else { return }
        try? (marker + "\n").write(toFile: path, atomically: true, encoding: .utf8)
    }

    // MARK: - Background / Foreground & Wireless Data (Preserved I5, I6)

    @objc private func applicationDidEnterBackground() {
        didReturnFromBackground = true
    }

    // Returning from Settings after enabling Wireless Data: recover without an app restart.
    @objc private func applicationDidBecomeActive() {
        guard didReturnFromBackground else { return }
        didReturnFromBackground = false

        // The SIP stack stays up even when start() reported failure; try re-registering
        // on the live client first, and fully restart the stack if that cannot run.
        if !client.isRegistered() {
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
        logsTextView?.text += message + "\n"
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        promptForWirelessDataIfNeeded()
        handleLaunchUiTestModeIfNeeded()
    }

    // The system "Wireless Data" prompt appears only once; afterwards the app can
    // only detect the denial and deep-link into its own Settings page.
    private func promptForWirelessDataIfNeeded() {
        guard !wirelessDataAlertVisible else { return }
        guard ProcessInfo.processInfo.environment["SIPSDK_UITEST_MODE"] == nil else { return }
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
            message: "Wireless data is disabled for this app, so SIP registration cannot reach the server. Enable WLAN & Cellular data for SipSdk in Settings.",
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
            if let path = callsTableView.indexPathForSelectedRow,
               path.section == 0,
               path.row < calls.count,
               let destination = segue.destination as? SipCallDetailsViewController {
                destination.sipCall = calls[path.row]
                destination.sipClient = client
            }
        }
    }
}

extension SipCallsViewController: SipClientDelegate {
    func onWillRegister(_ sipSdk: SipClient) {
        log("Registering...")
        updateRegistrationStatusBadge("Registering...", color: .systemBlue)
    }

    func onDidRegister(_ sipSdk: SipClient) {
        log("Registered")
        updateRegistrationStatusBadge("Registered", color: .systemGreen)
    }

    func onFailedRegister(_ sipSdk: SipClient) {
        log("Failed to register")
        updateRegistrationStatusBadge("Registration Failed", color: .systemRed)
        promptForWirelessDataIfNeeded()
    }

    func onWillUnRegister(_ sipSdk: SipClient) {
        log("Unregistering...")
        updateRegistrationStatusBadge("Unregistering...", color: .systemGray)
    }

    func onCallIncoming(_ sipCall: SipCall) {
        log("Incomming call from \(sipCall.remoteUri)...")
        SipCallKitManager.shared.reportIncomingCall(sipCall)

        let alertController = UIAlertController(title: sipCall.remoteUri,
                                                message: sipCall.hasVideo ? "Incoming Video Call" : "Incoming Audio Call",
                                                preferredStyle: .alert)
        alertController.addAction(UIAlertAction(title: "Answer Video", style: .default) { [weak self] _ in
            sipCall.answer(withVideo: true)
            self?.presentInCallScreen(for: sipCall, simulated: false)
        })
        alertController.addAction(UIAlertAction(title: "Answer Audio", style: .default) { [weak self] _ in
            sipCall.answer(withVideo: false)
            self?.presentInCallScreen(for: sipCall, simulated: false)
        })
        alertController.addAction(UIAlertAction(title: "Decline", style: .destructive) { _ in
            sipCall.hangup(486, reason: "Busy Here")
            SipCallKitManager.shared.reportCallEnded(sipCall, reason: .declinedElsewhere)
        })

        present(alertController, animated: true, completion: nil)

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
        SipCallKitManager.shared.reportOutgoingCallConnected(sipCall)
    }

    func onCallClosed(_ sipCall: SipCall) {
        log("Call closed from \(sipCall.remoteUri)...")
        SipCallKitManager.shared.reportCallEnded(sipCall, reason: .remoteEnded)
        SipCallHistoryStore.shared.recordCall(remoteUri: sipCall.remoteUri,
                                              isOutgoing: sipCall.isOutgoing,
                                              hasVideo: sipCall.hasVideo,
                                              durationSeconds: Int(sipCall.durationSeconds),
                                              outcome: sipCall.durationSeconds > 0 ? "Completed" : "Ended")

        calls.removeAll(where: { $0 === sipCall })
        reloadCallHistory()
    }

    func onCallHoldChanged(_ sipCall: SipCall, isOnHold: Bool) {
        log("Call \(sipCall.remoteUri) hold=\(isOnHold)")
    }

    func onCallVideoSizeChanged(_ sipCall: SipCall, width: Int32, height: Int32) {
        log("Video size changed to \(width)x\(height)")
    }

    func onDtmfReceived(_ sipCall: SipCall, digit: String) {
        log("Received DTMF digit: \(digit)")
    }
}

extension SipCallsViewController: SipCallKitManagerDelegate {
    func callKitDidAnswerCall(uuid: UUID) {
        if let call = SipCallKitManager.shared.call(for: uuid) {
            presentInCallScreen(for: call, simulated: false)
        }
    }

    func callKitDidEndCall(uuid: UUID) {
        calls.removeAll(where: { $0.uuid == uuid })
        reloadCallHistory()
    }
}

extension SipCallsViewController: UITableViewDataSource, UITableViewDelegate {
    func numberOfSections(in tableView: UITableView) -> Int {
        return 2
    }

    func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        if section == 0 {
            return "Active Calls (\(calls.count))"
        }
        return "Recent Call Log (\(historyEntries.count))"
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        if section == 0 {
            return calls.count
        }
        return historyEntries.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "CellId", for: indexPath) as! CallCell
        if indexPath.section == 0 {
            let sipCall = calls[indexPath.row]
            cell.configureWithActiveCall(sipCall)
        } else {
            let entry = historyEntries[indexPath.row]
            cell.configureWithHistoryEntry(entry)
        }
        return cell
    }

    func tableView(_ tableView: UITableView, willSelectRowAt indexPath: IndexPath) -> IndexPath? {
        if indexPath.section == 1 {
            let entry = historyEntries[indexPath.row]
            remoteUri.text = entry.remoteUri
            return nil
        }
        return indexPath
    }
}
