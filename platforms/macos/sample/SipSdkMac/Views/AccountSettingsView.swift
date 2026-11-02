import SwiftUI

public struct AccountSettingsView: View {
    @ObservedObject var appModel: SipAppModel
    @ObservedObject var accountStore: SipAccountStore

    public init(appModel: SipAppModel) {
        self.appModel = appModel
        self.accountStore = appModel.accountStore
    }

    public var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                // Title
                Text("SIP Account & Media Settings")
                    .font(.title2)
                    .fontWeight(.bold)

                // Registration Status Card
                VStack(alignment: .leading, spacing: 8) {
                    Text("Registration Status")
                        .font(.headline)

                    HStack(spacing: 12) {
                        Circle()
                            .fill(statusColor)
                            .frame(width: 12, height: 12)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(statusTitle)
                                .font(.body)
                                .fontWeight(.medium)

                            if !appModel.registrationReason.isEmpty {
                                Text("Reason: \(appModel.registrationReason) (code \(appModel.registrationCode))")
                                    .font(.caption)
                                    .foregroundColor(.secondary)
                            }
                        }

                        Spacer()

                        Button("Register") {
                            appModel.registerAccount()
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.green)

                        Button("Unregister") {
                            appModel.unregisterAccount()
                        }
                        .buttonStyle(.bordered)
                    }
                    .padding()
                    .background(Color(NSColor.controlBackgroundColor))
                    .cornerRadius(10)
                }

                // Account Credentials Section
                VStack(alignment: .leading, spacing: 12) {
                    Text("SIP Credentials")
                        .font(.headline)

                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 12) {
                        GridRow {
                            Text("Username:")
                                .foregroundColor(.secondary)
                            TextField("Username", text: $accountStore.username)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Domain / Host:")
                                .foregroundColor(.secondary)
                            TextField("SIP Domain or IP", text: $accountStore.domain)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Password:")
                                .foregroundColor(.secondary)
                            SecureField("SIP Password", text: $accountStore.password)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Port:")
                                .foregroundColor(.secondary)
                            TextField("5060", value: $accountStore.port, format: .number)
                                .textFieldStyle(.roundedBorder)
                        }

                        GridRow {
                            Text("Transport:")
                                .foregroundColor(.secondary)
                            Picker("", selection: $accountStore.transport) {
                                Text("UDP").tag("udp")
                                Text("TCP").tag("tcp")
                            }
                            .pickerStyle(.segmented)
                        }
                    }
                    .padding()
                    .background(Color(NSColor.controlBackgroundColor))
                    .cornerRadius(10)
                }

                // Video & Camera Subsystem
                VStack(alignment: .leading, spacing: 12) {
                    Text("Video & Camera Subsystem")
                        .font(.headline)

                    VStack(alignment: .leading, spacing: 8) {
                        Text("Active Camera Device:")
                            .foregroundColor(.secondary)

                        Picker("", selection: $appModel.selectedCamera) {
                            ForEach(appModel.availableCameras, id: \.self) { camera in
                                Text(camera).tag(camera)
                            }
                        }
                        .pickerStyle(.menu)
                        .onChange(of: appModel.selectedCamera) { newCamera in
                            appModel.selectCamera(name: newCamera)
                        }

                        Text("Select a physical webcam (FaceTime HD / External USB) or use the automated 30 fps synthetic test pattern generator.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    .padding()
                    .background(Color(NSColor.controlBackgroundColor))
                    .cornerRadius(10)
                }

                // Reset Action
                HStack {
                    Spacer()
                    Button("Reset Credentials to Defaults") {
                        accountStore.resetToDefaults()
                    }
                    .buttonStyle(.bordered)
                    .tint(.secondary)
                }
            }
            .padding(24)
        }
        .background(Color(NSColor.windowBackgroundColor))
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
            return "Registered as \(accountStore.username)@\(accountStore.domain):\(accountStore.port)"
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
}
