//
//  SettingsView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct SettingsView: View {
    @Bindable var model: SipAppModel
    @Environment(\.dismiss) private var dismiss

    // Editable draft state
    @State private var username: String = ""
    @State private var domain: String = ""
    @State private var password: String = ""
    @State private var displayName: String = ""
    @State private var transport: SipTransport = .udp
    @State private var portString: String = "5060"
    @State private var outboundProxy: String = ""
    @State private var stunServer: String = ""
    @State private var useIce: Bool = false
    @State private var regIntervalString: String = "3600"
    @State private var verifyTlsCert: Bool = true
    @State private var tlsCaFile: String = ""
    @State private var mediaEnc: String = "none"

    // Diagnostics / Handover state
    @State private var handoverMessage: String?
    @State private var isHandoverInProgress: Bool = false
    @State private var isPreviewActive: Bool = false
    @State private var previewVideoUIView = SipVideoUIView()

    var body: some View {
        NavigationStack {
            Form {
                // Section 1: Registration Status
                Section {
                    HStack(spacing: 12) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 12, height: 12)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(model.client.registrationState.description)
                                .font(.system(size: 15, weight: .semibold))
                            if let aor = model.client.currentAccount?.aor {
                                Text(aor)
                                    .font(.system(size: 12))
                                    .foregroundColor(.secondary)
                            }
                        }
                        Spacer()

                        if model.client.registrationState.isRegistered {
                            Button("Unregister") {
                                Task {
                                    await model.client.unregister()
                                }
                            }
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(.red)
                        } else {
                            Button("Register") {
                                saveAndRegister()
                            }
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundColor(.blue)
                        }
                    }
                    .padding(.vertical, 4)
                } header: {
                    Text("SIP Account Status")
                }

                // Section 2: Account Credentials
                Section {
                    HStack {
                        Text("Username")
                            .frame(width: 100, alignment: .leading)
                        TextField("e.g. 1001", text: $username)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }

                    HStack {
                        Text("Domain / Host")
                            .frame(width: 100, alignment: .leading)
                        TextField("e.g. 127.0.0.1", text: $domain)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }

                    HStack {
                        Text("Password")
                            .frame(width: 100, alignment: .leading)
                        SecureField("Required", text: $password)
                    }

                    HStack {
                        Text("Display Name")
                            .frame(width: 100, alignment: .leading)
                        TextField("Optional", text: $displayName)
                    }
                } header: {
                    Text("Credentials")
                }

                // Section 3: Connection & Transport
                Section {
                    Picker("Transport", selection: $transport) {
                        Text("UDP").tag(SipTransport.udp)
                        Text("TCP").tag(SipTransport.tcp)
                        Text("TLS").tag(SipTransport.tls)
                        Text("WS").tag(SipTransport.ws)
                        Text("WSS").tag(SipTransport.wss)
                    }
                    .onChange(of: transport) { _, newValue in
                        if newValue == .tls {
                            if portString == "5060" { portString = "5061" }
                            if mediaEnc == "none" { mediaEnc = "srtp-mand" }
                        } else if newValue == .ws {
                            if portString == "5060" || portString == "5061" { portString = "8088" }
                        } else if newValue == .wss {
                            if portString == "5060" || portString == "5061" || portString == "8088" { portString = "8089" }
                            if mediaEnc == "none" { mediaEnc = "srtp-mand" }
                        }
                    }

                    Picker("Media Encryption", selection: $mediaEnc) {
                        Text("None (Plain RTP)").tag("none")
                        Text("SRTP (Optional)").tag("srtp")
                        Text("SRTP (Mandatory)").tag("srtp-mand")
                        Text("DTLS-SRTP").tag("dtls_srtp")
                    }

                    if transport == .tls || transport == .wss {
                        Toggle("Verify TLS Certificate", isOn: $verifyTlsCert)
                        HStack {
                            Text("CA Cert File")
                                .frame(width: 100, alignment: .leading)
                            TextField("Optional PEM path", text: $tlsCaFile)
                                .autocorrectionDisabled()
                                .textInputAutocapitalization(.never)
                        }
                    }

                    HStack {
                        Text("Port")
                            .frame(width: 100, alignment: .leading)
                        TextField("5060", text: $portString)
                            .keyboardType(.numberPad)
                    }

                    HStack {
                        Text("Outbound Proxy")
                            .frame(width: 100, alignment: .leading)
                        TextField("Optional (sip:host:port)", text: $outboundProxy)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }

                    HStack {
                        Text("STUN Server")
                            .frame(width: 100, alignment: .leading)
                        TextField("Optional (stun.l.google.com)", text: $stunServer)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }

                    Toggle("Enable ICE NAT Traversal", isOn: $useIce)

                    HStack {
                        Text("Reg Interval")
                            .frame(width: 100, alignment: .leading)
                        TextField("3600", text: $regIntervalString)
                            .keyboardType(.numberPad)
                    }
                } header: {
                    Text("Network & NAT")
                }

                // Section 4: Enterprise Network Handover
                Section {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Reset Transports simulates seamless switching between Wi-Fi and Cellular by re-binding sockets and refreshing SIP registration without dropping the engine.")
                            .font(.system(size: 12))
                            .foregroundColor(.secondary)

                        Button {
                            triggerNetworkHandover()
                        } label: {
                            HStack {
                                Image(systemName: "arrow.triangle.2.circlepath")
                                Text(isHandoverInProgress ? "Re-binding Transports..." : "Perform Network Handover")
                                    .fontWeight(.semibold)
                            }
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(isHandoverInProgress || !model.client.isStarted)

                        if let msg = handoverMessage {
                            Text(msg)
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(.green)
                        }
                    }
                    .padding(.vertical, 4)
                } header: {
                    Text("Enterprise Features")
                } footer: {
                    Text("SipSdk provides native C ABI `sip_client_reset_transports` for zero-downtime mobile handover.")
                }

                // Section 5: Audio & Media
                Section {
                    Picker("Camera", selection: Binding(
                        get: { model.client.cameraPosition },
                        set: { newPos in
                            model.client.cameraPosition = newPos
                        }
                    )) {
                        Text("Front (TrueDepth)").tag(SipCameraPosition.front)
                        Text("Back (Wide)").tag(SipCameraPosition.back)
                    }

                    Toggle("Speakerphone Default", isOn: Binding(
                        get: { model.client.isSpeakerEnabled },
                        set: { val in
                            model.client.isSpeakerEnabled = val
                        }
                    ))

                    Toggle("Local Camera Preview Test", isOn: $isPreviewActive)
                        .onChange(of: isPreviewActive) { _, active in
                            Task {
                                if active {
                                    if !model.client.isStarted {
                                        try? await model.client.start()
                                    }
                                    model.client.localVideoView = previewVideoUIView
                                    model.client.startLocalVideoPreview()
                                } else {
                                    model.client.stopLocalVideoPreview()
                                }
                            }
                        }

                    if isPreviewActive {
                        VStack(alignment: .center, spacing: 8) {
                            SipVideoView(videoUIView: previewVideoUIView)
                                .frame(height: 180)
                                .clipShape(RoundedRectangle(cornerRadius: 12))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 12)
                                        .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
                                )
                            Text("Real-time VideoToolbox H.264 Local View")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                        }
                        .padding(.vertical, 6)
                    }
                } header: {
                    Text("Media Hardware")
                }

                // Section 6: Reset / Restore
                Section {
                    Button("Reset to Defaults", role: .destructive) {
                        model.accountStore.resetToDefaults()
                        loadFromStore()
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Cancel") {
                        dismiss()
                    }
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Button("Save") {
                        saveAndRegister()
                        dismiss()
                    }
                    .fontWeight(.bold)
                }
            }
            .onAppear {
                loadFromStore()
            }
        }
    }

    private var statusColor: Color {
        switch model.client.registrationState {
        case .registered: return .green
        case .registering: return .orange
        case .failed: return .red
        case .unregistered: return .gray
        }
    }

    private func loadFromStore() {
        let acc = model.accountStore.account
        self.username = acc.username
        self.domain = acc.domain
        self.password = acc.password
        self.displayName = acc.displayName ?? ""
        self.transport = acc.transport
        self.portString = "\(acc.port)"
        self.outboundProxy = acc.outboundProxy ?? ""
        self.stunServer = acc.stunServer ?? ""
        self.useIce = acc.mediaNat == "ice"
        self.regIntervalString = "\(acc.regInterval)"
        self.verifyTlsCert = acc.verifyTlsCert
        self.tlsCaFile = acc.tlsCaFile ?? ""
        self.mediaEnc = acc.mediaEnc ?? ((acc.transport == .tls || acc.transport == .wss) ? "srtp-mand" : "none")
    }

    private func saveAndRegister() {
        let defaultPort: Int
        switch transport {
        case .tls: defaultPort = 5061
        case .ws: defaultPort = 8088
        case .wss: defaultPort = 8089
        case .udp, .tcp: defaultPort = 5060
        }
        let port = Int(portString) ?? defaultPort
        let regInterval = UInt32(regIntervalString) ?? 3600

        let updated = SipAccount(
            username: username.trimmingCharacters(in: .whitespaces),
            domain: domain.trimmingCharacters(in: .whitespaces),
            password: password,
            transport: transport,
            port: port,
            displayName: displayName.isEmpty ? nil : displayName,
            outboundProxy: outboundProxy.isEmpty ? nil : outboundProxy,
            stunServer: stunServer.isEmpty ? nil : stunServer,
            mediaNat: useIce ? "ice" : nil,
            regInterval: regInterval,
            tlsCaFile: tlsCaFile.isEmpty ? nil : tlsCaFile,
            verifyTlsCert: verifyTlsCert,
            mediaEnc: mediaEnc == "none" ? nil : mediaEnc
        )

        model.accountStore.account = updated
        Task {
            await model.startAndRegister()
        }
    }

    private func triggerNetworkHandover() {
        isHandoverInProgress = true
        handoverMessage = nil
        model.client.resetTransports(reRegister: true)
        handoverMessage = "Transports reset & re-registration initiated"
        isHandoverInProgress = false
    }
}
