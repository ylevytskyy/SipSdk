import SwiftUI

public struct CallHistoryView: View {
    @ObservedObject var appModel: SipAppModel

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Text("Recent Calls")
                    .font(.title2)
                    .fontWeight(.bold)

                Spacer()

                if !appModel.historyStore.records.isEmpty {
                    Button("Clear History") {
                        appModel.historyStore.clear()
                    }
                    .buttonStyle(.bordered)
                    .tint(.secondary)
                }
            }
            .padding()

            Divider()

            if appModel.historyStore.records.isEmpty {
                VStack(spacing: 12) {
                    Spacer()
                    Image(systemName: "clock")
                        .font(.system(size: 48))
                        .foregroundColor(.secondary)
                    Text("No Call History")
                        .font(.headline)
                        .foregroundColor(.secondary)
                    Text("Completed incoming and outgoing calls will appear here.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Spacer()
                }
            } else {
                List {
                    ForEach(appModel.historyStore.records) { record in
                        HStack(spacing: 12) {
                            Image(systemName: record.direction == "incoming"
                                  ? "arrow.down.left.circle.fill"
                                  : "arrow.up.right.circle.fill")
                                .font(.title2)
                                .foregroundColor(record.direction == "incoming" ? .blue : .green)

                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 6) {
                                    Text(record.remoteUri)
                                        .font(.headline)

                                    if record.hasVideo {
                                        Image(systemName: "video.fill")
                                            .font(.caption)
                                            .foregroundColor(.secondary)
                                    }
                                }

                                HStack(spacing: 8) {
                                    Text(record.status)
                                        .font(.caption)
                                        .foregroundColor(statusColor(record.status))

                                    Text("•")
                                        .font(.caption)
                                        .foregroundColor(.secondary)

                                    Text(formatDuration(record.durationSeconds))
                                        .font(.caption)
                                        .foregroundColor(.secondary)

                                    Text("•")
                                        .font(.caption)
                                        .foregroundColor(.secondary)

                                    Text(formatDate(record.timestamp))
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }

                            Spacer()

                            Button(action: {
                                appModel.makeCall(targetUri: record.remoteUri, withVideo: false)
                            }) {
                                Image(systemName: "phone.fill")
                                    .foregroundColor(.green)
                            }
                            .buttonStyle(.plain)
                            .help("Redial Audio Call")

                            if record.hasVideo {
                                Button(action: {
                                    appModel.makeCall(targetUri: record.remoteUri, withVideo: true)
                                }) {
                                    Image(systemName: "video.fill")
                                        .foregroundColor(.blue)
                                }
                                .buttonStyle(.plain)
                                .help("Redial Video Call")
                            }
                        }
                        .padding(.vertical, 4)
                    }
                }
            }
        }
        .background(Color(NSColor.windowBackgroundColor))
    }

    private func formatDuration(_ seconds: UInt32) -> String {
        let mins = seconds / 60
        let secs = seconds % 60
        return String(format: "%02d:%02d", mins, secs)
    }

    private func formatDate(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateStyle = .short
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    private func statusColor(_ status: String) -> Color {
        switch status.lowercased() {
        case "completed", "answered":
            return .green
        case "missed", "declined":
            return .red
        default:
            return .secondary
        }
    }
}
