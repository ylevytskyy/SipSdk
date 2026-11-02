//
//  HomeView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct HomeView: View {
    @Bindable var model: SipAppModel
    @FocusState private var isInputFocused: Bool

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    headerSection
                    statusBadgeSection
                    dialerSection
                    recentCallsSection
                    quickCameraCheckSection
                }
                .padding(.horizontal, 20)
                .padding(.top, 10)
                .padding(.bottom, 30)
            }
            .navigationTitle("SipSdk")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.isShowingAccountSheet = true
                    } label: {
                        Image(systemName: "gearshape")
                            .font(.system(size: 17, weight: .semibold))
                    }
                }
            }
        }
    }

    // MARK: - Subviews

    private var headerSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("SipSdk")
                .font(.system(size: 32, weight: .bold))
                .foregroundColor(.primary)

            Text("Who would you like to call?")
                .font(.system(size: 16))
                .foregroundColor(.secondary)
        }
    }

    private var statusBadgeSection: some View {
        Button {
            model.isShowingAccountSheet = true
        } label: {
            HStack(spacing: 12) {
                Circle()
                    .fill(statusColor)
                    .frame(width: 10, height: 10)

                VStack(alignment: .leading, spacing: 2) {
                    Text(statusTitle)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.primary)

                    Text(model.accountStore.account.aor)
                        .font(.system(size: 13))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                }

                Spacer()

                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundColor(Color(uiColor: .tertiaryLabel))
            }
            .padding(16)
            .background(Color(uiColor: .secondarySystemGroupedBackground))
            .cornerRadius(14)
            .shadow(color: Color.black.opacity(0.04), radius: 6, x: 0, y: 2)
        }
        .buttonStyle(.plain)
    }

    private var dialerSection: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Number or SIP address")
                    .font(.system(size: 14, weight: .medium))
                    .foregroundColor(.secondary)

                HStack {
                    TextField("1002", text: $model.dialDestination)
                        .font(.system(size: 20, weight: .semibold))
                        .keyboardType(.numbersAndPunctuation)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .focused($isInputFocused)

                    if !model.dialDestination.isEmpty {
                        Button {
                            model.dialDestination = ""
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .padding(14)
                .background(Color(uiColor: .secondarySystemGroupedBackground))
                .cornerRadius(12)
                .overlay(
                    RoundedRectangle(cornerRadius: 12)
                        .stroke(Color(uiColor: .separator), lineWidth: 0.5)
                )

                Text("Numbers use your account's domain.")
                    .font(.system(size: 12))
                    .foregroundColor(Color(uiColor: .tertiaryLabel))
            }

            VStack(spacing: 10) {
                Button {
                    isInputFocused = false
                    Task {
                        await model.makeCall(to: model.dialDestination, withVideo: true)
                    }
                } label: {
                    HStack {
                        Image(systemName: "video.fill")
                        Text("Video call")
                            .font(.system(size: 17, weight: .semibold))
                    }
                    .frame(maxWidth: .infinity)
                    .frame(height: 52)
                    .background(Color.blue)
                    .foregroundColor(.white)
                    .cornerRadius(14)
                }
                .disabled(model.dialDestination.isEmpty)

                Button {
                    isInputFocused = false
                    Task {
                        await model.makeCall(to: model.dialDestination, withVideo: false)
                    }
                } label: {
                    HStack {
                        Image(systemName: "phone.fill")
                        Text("Audio call")
                            .font(.system(size: 17, weight: .semibold))
                    }
                    .frame(maxWidth: .infinity)
                    .frame(height: 52)
                    .background(Color(uiColor: .secondarySystemGroupedBackground))
                    .foregroundColor(.blue)
                    .cornerRadius(14)
                    .overlay(
                        RoundedRectangle(cornerRadius: 14)
                            .stroke(Color.blue.opacity(0.3), lineWidth: 1)
                    )
                }
                .disabled(model.dialDestination.isEmpty)
            }
        }
    }

    private var recentCallsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Recent calls")
                    .font(.system(size: 18, weight: .bold))

                Spacer()

                Button("See all") {
                    model.selectedTab = 1
                }
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(.blue)
            }

            if model.history.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "phone.badge.plus")
                        .font(.system(size: 28))
                        .foregroundColor(.secondary)
                    Text("No calls yet")
                        .font(.system(size: 14))
                        .foregroundColor(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 24)
                .background(Color(uiColor: .secondarySystemGroupedBackground))
                .cornerRadius(12)
            } else {
                VStack(spacing: 0) {
                    ForEach(model.history.prefix(3)) { item in
                        Button {
                            model.dialDestination = item.remoteUri
                            Task {
                                await model.makeCall(to: item.remoteUri, withVideo: item.hasVideo)
                            }
                        } label: {
                            HStack(spacing: 12) {
                                Image(systemName: item.direction == .outgoing ? "arrow.up.right" : "arrow.down.left")
                                    .foregroundColor(item.direction == .missed ? .red : .blue)
                                    .font(.system(size: 14, weight: .bold))

                                VStack(alignment: .leading, spacing: 2) {
                                    Text(item.displayTitle)
                                        .font(.system(size: 15, weight: .semibold))
                                        .foregroundColor(.primary)

                                    HStack(spacing: 6) {
                                        if item.hasVideo {
                                            Label("Video", systemImage: "video")
                                                .font(.system(size: 12))
                                        }
                                        Text(item.formattedDuration)
                                            .font(.system(size: 12))
                                        Text("·")
                                        Text(item.formattedDate)
                                            .font(.system(size: 12))
                                    }
                                    .foregroundColor(.secondary)
                                }

                                Spacer()

                                Image(systemName: "chevron.right")
                                    .font(.system(size: 12, weight: .semibold))
                                    .foregroundColor(Color(uiColor: .tertiaryLabel))
                            }
                            .padding(.vertical, 12)
                            .padding(.horizontal, 14)
                        }
                        .buttonStyle(.plain)

                        if item.id != model.history.prefix(3).last?.id {
                            Divider()
                                .padding(.leading, 42)
                        }
                    }
                }
                .background(Color(uiColor: .secondarySystemGroupedBackground))
                .cornerRadius(12)
            }
        }
    }

    private var quickCameraCheckSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Check your camera first")
                .font(.system(size: 16, weight: .semibold))

            Text("Preview stays on this device.")
                .font(.system(size: 13))
                .foregroundColor(.secondary)

            Button {
                model.selectedTab = 2
            } label: {
                Text("Open video settings")
                    .font(.system(size: 15, weight: .semibold))
                    .frame(maxWidth: .infinity)
                    .frame(height: 44)
                    .background(Color.blue.opacity(0.1))
                    .foregroundColor(.blue)
                    .cornerRadius(10)
            }
        }
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground))
        .cornerRadius(14)
    }

    private var statusColor: Color {
        switch model.client.registrationState {
        case .registered: return .green
        case .registering: return .yellow
        case .failed: return .red
        case .unregistered: return .gray
        }
    }

    private var statusTitle: String {
        switch model.client.registrationState {
        case .registered: return "Registered · Ready to call"
        case .registering: return "Connecting to SIP server..."
        case .failed(_, let reason): return "Registration Failed (\(reason))"
        case .unregistered: return "Offline · Not registered"
        }
    }
}
