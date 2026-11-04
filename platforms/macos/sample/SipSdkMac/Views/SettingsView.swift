import SwiftUI

public struct SettingsView: View {
    @ObservedObject var appModel: SipAppModel
    @ObservedObject var accountStore: SipAccountStore

    // Draft state for account configuration
    @State private var username: String = ""
    @State private var domain: String = ""
    @State private var password: String = ""
    @State private var displayName: String = ""
    @State private var transport: String = "udp"
    @State private var portString: String = "5060"
    @State private var outboundProxy: String = ""
    @State private var stunServer: String = ""
    @State private var useIce: Bool = false
    @State private var regIntervalString: String = "3600"

    public init(appModel: SipAppModel) {
        self.appModel = appModel
        self.accountStore = appModel.accountStore
    }

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Header
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Settings")
                            .font(.system(size: 24, weight: .bold))
                        Text("SIP Account, Network, and Audio/Video Media Devices")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                    }
                    Spacer()
                    Button("Save & Register") {
                        saveAndRegister()
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.blue)
                }

                // Section 1: SIP Account Status
                sectionCard(title: "SIP Account Status") {
                    HStack(spacing: 14) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 12, height: 12)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(statusTitle)
                                .font(.system(size: 14, weight: .semibold))

                            Text("sip:\(username.isEmpty ? accountStore.username : username)@\(domain.isEmpty ? accountStore.domain : domain):\(portString)")
                                .font(.system(size: 12))
                                .foregroundColor(.secondary)

                            if !appModel.registrationReason.isEmpty {
                                Text("Reason: \(appModel.registrationReason) (code \(appModel.registrationCode))")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                        }

                        Spacer()

                        if appModel.registrationState == .registered {
                            Button("Unregister") {
                                appModel.unregisterAccount()
                            }
                            .buttonStyle(.bordered)
                            .tint(.red)
                        } else {
                            Button("Register") {
                                saveAndRegister()
                            }
                            .buttonStyle(.borderedProminent)
                            .tint(.green)
                        }
                    }
                }

                // Section 2: Account Credentials
                sectionCard(title: "Credentials") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                        GridRow {
                            Text("Username:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("e.g. 1001", text: $username)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Domain / Host:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("e.g. 127.0.0.1", text: $domain)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Password:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            SecureField("Required", text: $password)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Display Name:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("Optional (e.g. Mac Sample)", text: $displayName)
                                .textFieldStyle(.roundedBorder)
                        }
                    }
                }

                // Section 3: Connection & Transport (Network & NAT)
                sectionCard(title: "Network & NAT") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                        GridRow {
                            Text("Transport:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            Picker("", selection: $transport) {
                                Text("UDP").tag("udp")
                                Text("TCP").tag("tcp")
                            }
                            .pickerStyle(.segmented)
                            .frame(maxWidth: 160)
                        }

                        GridRow {
                            Text("Port:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("5060", text: $portString)
                                .textFieldStyle(.roundedBorder)
                                .frame(maxWidth: 160)
                        }

                        GridRow {
                            Text("Outbound Proxy:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("Optional (sip:proxy.example.com:5060)", text: $outboundProxy)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("STUN Server:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("Optional (stun.l.google.com:19302)", text: $stunServer)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("ICE NAT Traversal:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            Toggle("Enable ICE NAT Traversal", isOn: $useIce)
                                .toggleStyle(.switch)
                        }

                        GridRow {
                            Text("Reg Interval:")
                                .foregroundColor(.secondary)
                                .frame(width: 120, alignment: .trailing)
                            TextField("3600", text: $regIntervalString)
                                .textFieldStyle(.roundedBorder)
                                .frame(maxWidth: 160)
                        }
                    }
                }

                // Section 4: Enterprise Network Handover
                sectionCard(title: "Enterprise Features") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Reset Transports simulates seamless switching between Wi-Fi and Ethernet by re-binding transport sockets and refreshing SIP registration without dropping the engine.")
                            .font(.system(size: 12))
                            .foregroundColor(.secondary)

                        HStack(spacing: 12) {
                            Button {
                                appModel.performNetworkHandover()
                            } label: {
                                HStack(spacing: 6) {
                                    Image(systemName: "arrow.triangle.2.circlepath")
                                    Text(appModel.isHandoverInProgress ? "Re-binding Transports..." : "Perform Network Handover")
                                }
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(appModel.isHandoverInProgress)

                            if let msg = appModel.handoverMessage {
                                Text(msg)
                                    .font(.system(size: 12, weight: .medium))
                                    .foregroundColor(.green)
                            }
                        }
                    }
                }

                // Section 5: Audio & Video Media Devices
                sectionCard(title: "Media Hardware & Devices") {
                    VStack(alignment: .leading, spacing: 14) {
                        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                            // Audio Input (Microphone)
                            GridRow {
                                Text("Microphone (In):")
                                    .foregroundColor(.secondary)
                                    .frame(width: 120, alignment: .trailing)
                                Picker("", selection: $appModel.selectedAudioInput) {
                                    ForEach(appModel.availableAudioInputs, id: \.self) { dev in
                                        Text(dev).tag(dev)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedAudioInput) { newDev in
                                    appModel.selectAudioInput(name: newDev)
                                }
                            }

                            // Audio Output (Speaker / Headphones)
                            GridRow {
                                Text("Speaker (Out):")
                                    .foregroundColor(.secondary)
                                    .frame(width: 120, alignment: .trailing)
                                Picker("", selection: $appModel.selectedAudioOutput) {
                                    ForEach(appModel.availableAudioOutputs, id: \.self) { dev in
                                        Text(dev).tag(dev)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedAudioOutput) { newDev in
                                    appModel.selectAudioOutput(name: newDev)
                                }
                            }

                            // Video Input (Camera)
                            GridRow {
                                Text("Camera (In):")
                                    .foregroundColor(.secondary)
                                    .frame(width: 120, alignment: .trailing)
                                Picker("", selection: $appModel.selectedCamera) {
                                    ForEach(appModel.availableCameras, id: \.self) { camera in
                                        Text(camera).tag(camera)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedCamera) { newCamera in
                                    appModel.selectCamera(name: newCamera)
                                }
                            }

                            // Video Output (Display / Monitor)
                            GridRow {
                                Text("Display (Out):")
                                    .foregroundColor(.secondary)
                                    .frame(width: 120, alignment: .trailing)
                                Picker("", selection: $appModel.selectedVideoOutput) {
                                    ForEach(appModel.availableVideoOutputs, id: \.self) { screen in
                                        Text(screen).tag(screen)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedVideoOutput) { newScreen in
                                    appModel.selectVideoOutput(name: newScreen)
                                }
                            }
                        }

                        Divider()
                            .padding(.vertical, 2)

                        // Local Camera Preview Test Toggle
                        HStack {
                            Text("Local Camera Preview Test")
                                .font(.system(size: 13, weight: .medium))
                            Spacer()
                            Toggle("", isOn: $appModel.isPreviewActive)
                                .toggleStyle(.switch)
                                .onChange(of: appModel.isPreviewActive) { active in
                                    appModel.setPreviewActive(active)
                                }
                        }

                        if appModel.isPreviewActive {
                            VStack(alignment: .center, spacing: 6) {
                                SipVideoViewRepresentable(viewType: .localPreview) { view, type in
                                    appModel.attachVideoView(view, type: type)
                                }
                                .frame(height: 180)
                                .frame(maxWidth: .infinity)
                                .background(Color.black)
                                .clipShape(RoundedRectangle(cornerRadius: 10))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 10)
                                        .stroke(Color.secondary.opacity(0.3), lineWidth: 1)
                                )

                                Text("Real-time VideoToolbox H.264 Local View")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                            }
                            .padding(.top, 4)
                        }
                    }
                }

                // Section 6: Reset / Restore
                HStack {
                    Spacer()
                    Button("Reset to Defaults", role: .destructive) {
                        accountStore.resetToDefaults()
                        loadFromStore()
                    }
                    .buttonStyle(.bordered)
                }
            }
            .padding(24)
        }
        .background(Color(NSColor.windowBackgroundColor))
        .onAppear {
            loadFromStore()
            appModel.refreshMediaDevices()
        }
    }

    // MARK: - Helpers

    @ViewBuilder
    private func sectionCard<Content: View>(title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.system(size: 14, weight: .bold))
                .foregroundColor(.primary)

            VStack(alignment: .leading, spacing: 10) {
                content()
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(NSColor.controlBackgroundColor))
            .cornerRadius(10)
        }
    }

    private var statusColor: Color {
        switch appModel.registrationState {
        case .registered:
            return .green
        case .registering:
            return .orange
        case .failed:
            return .red
        case .unregistered:
            return .gray
        @unknown default:
            return .gray
        }
    }

    private var statusTitle: String {
        switch appModel.registrationState {
        case .registered:
            return "Registered as \(username)@\(domain):\(portString)"
        case .registering:
            return "Attempting registration..."
        case .failed:
            return "Registration Failed"
        case .unregistered:
            return "Unregistered / Offline"
        @unknown default:
            return "Unknown"
        }
    }

    private func loadFromStore() {
        self.username = accountStore.username
        self.domain = accountStore.domain
        self.password = accountStore.password
        self.displayName = accountStore.displayName
        self.transport = accountStore.transport
        self.portString = "\(accountStore.port)"
        self.outboundProxy = accountStore.outboundProxy
        self.stunServer = accountStore.stunServer
        self.useIce = accountStore.useIce
        self.regIntervalString = "\(accountStore.regInterval)"
    }

    private func saveAndRegister() {
        let p = Int(portString) ?? 5060
        let interval = UInt32(regIntervalString) ?? 3600

        accountStore.username = username.trimmingCharacters(in: .whitespaces)
        accountStore.domain = domain.trimmingCharacters(in: .whitespaces)
        accountStore.password = password
        accountStore.displayName = displayName.trimmingCharacters(in: .whitespaces)
        accountStore.transport = transport
        accountStore.port = p
        accountStore.outboundProxy = outboundProxy.trimmingCharacters(in: .whitespaces)
        accountStore.stunServer = stunServer.trimmingCharacters(in: .whitespaces)
        accountStore.useIce = useIce
        accountStore.regInterval = interval

        appModel.registerAccount()
    }
}
