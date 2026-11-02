//
//  CallHistoryView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct CallHistoryView: View {
    @Bindable var model: SipAppModel

    var body: some View {
        NavigationStack {
            Group {
                if model.history.isEmpty {
                    ContentUnavailableView(
                        "No Recent Calls",
                        systemImage: "phone.badge.clock",
                        description: Text("Calls you make or receive will appear here.")
                    )
                } else {
                    List {
                        ForEach(model.history) { item in
                            CallHistoryRow(item: item) {
                                Task {
                                    model.dialDestination = item.remoteUri
                                    await model.makeCall(to: item.remoteUri, withVideo: item.hasVideo)
                                }
                            }
                        }
                        .onDelete(perform: deleteItems)
                    }
                    .listStyle(.insetGrouped)
                }
            }
            .navigationTitle("Recents")
            .toolbar {
                if !model.history.isEmpty {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("Clear", role: .destructive) {
                            model.clearHistory()
                        }
                    }
                }
            }
        }
    }

    private func deleteItems(at offsets: IndexSet) {
        model.history.remove(atOffsets: offsets)
    }
}

private struct CallHistoryRow: View {
    let item: CallHistoryItem
    let onCallback: () -> Void

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: iconName)
                .font(.system(size: 18))
                .foregroundColor(iconColor)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(item.displayName ?? item.remoteUri)
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundColor(.primary)

                    if item.hasVideo {
                        Image(systemName: "video.fill")
                            .font(.system(size: 11))
                            .foregroundColor(.secondary)
                    }
                }

                if item.displayName != nil && !item.remoteUri.isEmpty {
                    Text(item.remoteUri)
                        .font(.system(size: 13))
                        .foregroundColor(.secondary)
                }

                HStack(spacing: 6) {
                    Text(item.formattedDate)
                        .font(.system(size: 12))
                        .foregroundColor(.secondary)

                    if item.durationSeconds > 0 {
                        Text("•")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Text(item.formattedDuration)
                            .font(.system(size: 12))
                            .foregroundColor(.secondary)
                    }
                }
            }

            Spacer()

            Button(action: onCallback) {
                Image(systemName: item.hasVideo ? "video.circle.fill" : "phone.circle.fill")
                    .font(.system(size: 28))
                    .foregroundColor(.green)
            }
            .buttonStyle(.plain)
        }
        .padding(.vertical, 4)
    }

    private var iconName: String {
        switch item.direction {
        case .incoming:
            return "phone.arrow.down.left"
        case .outgoing:
            return "phone.arrow.up.right"
        case .missed:
            return "phone.arrow.down.left.fill"
        }
    }

    private var iconColor: Color {
        switch item.direction {
        case .incoming:
            return .blue
        case .outgoing:
            return .green
        case .missed:
            return .red
        }
    }
}
