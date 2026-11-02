import SwiftUI

public struct DialerView: View {
    @ObservedObject var appModel: SipAppModel
    @State private var targetUri: String = "9001"

    private let keypadRows = [
        ["1", "2", "3"],
        ["4", "5", "6"],
        ["7", "8", "9"],
        ["*", "0", "#"]
    ]

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        VStack(spacing: 20) {
            // Registration status pill
            HStack {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)
                Text(statusText)
                    .font(.caption)
                    .foregroundColor(.secondary)
                Spacer()
                Text(appModel.accountStore.username + "@" + appModel.accountStore.domain)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 24)
            .padding(.top, 16)

            Spacer()

            // Target URI display
            VStack(spacing: 8) {
                HStack {
                    TextField("Enter extension or sip:user@domain", text: $targetUri)
                        .font(.system(size: 24, weight: .medium, design: .monospaced))
                        .multilineTextAlignment(.center)
                        .textFieldStyle(.plain)

                    if !targetUri.isEmpty {
                        Button(action: {
                            if !targetUri.isEmpty {
                                targetUri.removeLast()
                            }
                        }) {
                            Image(systemName: "delete.left.fill")
                                .font(.title3)
                                .foregroundColor(.secondary)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 32)

                Divider()
                    .padding(.horizontal, 48)
            }

            // Keypad Grid
            VStack(spacing: 12) {
                ForEach(keypadRows, id: \.self) { row in
                    HStack(spacing: 16) {
                        ForEach(row, id: \.self) { digit in
                            Button(action: {
                                targetUri.append(digit)
                            }) {
                                Text(digit)
                                    .font(.system(size: 24, weight: .regular))
                                    .frame(width: 68, height: 50)
                                    .background(Color(NSColor.controlBackgroundColor))
                                    .foregroundColor(.primary)
                                    .cornerRadius(10)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }

            Spacer()

            // Call Action Buttons
            HStack(spacing: 24) {
                Button(action: {
                    appModel.makeCall(targetUri: targetUri, withVideo: false)
                }) {
                    HStack(spacing: 8) {
                        Image(systemName: "phone.fill")
                        Text("Audio Call")
                    }
                    .font(.headline)
                    .foregroundColor(.white)
                    .frame(minWidth: 140, minHeight: 44)
                    .background(Color.green)
                    .cornerRadius(22)
                }
                .buttonStyle(.plain)
                .disabled(targetUri.isEmpty)

                Button(action: {
                    appModel.makeCall(targetUri: targetUri, withVideo: true)
                }) {
                    HStack(spacing: 8) {
                        Image(systemName: "video.fill")
                        Text("Video Call")
                    }
                    .font(.headline)
                    .foregroundColor(.white)
                    .frame(minWidth: 140, minHeight: 44)
                    .background(Color.blue)
                    .cornerRadius(22)
                }
                .buttonStyle(.plain)
                .disabled(targetUri.isEmpty)
            }
            .padding(.bottom, 24)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
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

    private var statusText: String {
        switch appModel.registrationState {
        case .registered:
            return "Registered (200 OK)"
        case .registering:
            return "Registering..."
        case .failed:
            return "Registration Failed: \(appModel.registrationReason)"
        case .unregistered:
            return "Offline (Unregistered)"
        @unknown default:
            return "Unknown"
        }
    }
}
