import SwiftUI

public enum NavigationSection: String, CaseIterable, Identifiable {
    case dialer = "Dialer"
    case history = "Recent Calls"
    case settings = "Settings"

    public var id: String { rawValue }

    public var iconName: String {
        switch self {
        case .dialer:
            return "phone.circle.fill"
        case .history:
            return "clock.fill"
        case .settings:
            return "gearshape.fill"
        }
    }
}

public struct MainSplitView: View {
    @ObservedObject var appModel: SipAppModel
    @State private var selectedSection: NavigationSection?
    @State private var columnVisibility: NavigationSplitViewVisibility = .all

    public init(appModel: SipAppModel) {
        self.appModel = appModel
        _selectedSection = State(initialValue: NavigationSection(rawValue: appModel.initialSectionName) ?? .dialer)
    }

    public var body: some View {
        ZStack(alignment: .top) {
            NavigationSplitView(columnVisibility: $columnVisibility) {
                List(NavigationSection.allCases, selection: $selectedSection) { section in
                    NavigationLink(value: section) {
                        Label(section.rawValue, systemImage: section.iconName)
                    }
                }
                .navigationSplitViewColumnWidth(min: 180, ideal: 200, max: 240)
                .listStyle(.sidebar)
            } detail: {
                if appModel.activeCall != nil {
                    InCallView(appModel: appModel)
                } else {
                    switch selectedSection {
                    case .dialer, .none:
                        DialerView(appModel: appModel)
                    case .history:
                        CallHistoryView(appModel: appModel)
                    case .settings:
                        SettingsView(appModel: appModel)
                    }
                }
            }

            // Top Incoming Call Alert Banner & Busy Toast
            VStack(spacing: 6) {
                if let busyMsg = appModel.busyBannerMessage {
                    Text(busyMsg)
                        .font(.caption.weight(.semibold))
                        .foregroundColor(.white)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 6)
                        .background(Color.red.opacity(0.9))
                        .cornerRadius(8)
                }
                IncomingCallBanner(appModel: appModel)
            }
            .padding(.top, 8)
        }
    }
}
