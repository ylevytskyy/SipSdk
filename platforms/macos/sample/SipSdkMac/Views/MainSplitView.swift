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
    @State private var selectedSection: NavigationSection? = .dialer

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        ZStack(alignment: .top) {
            NavigationSplitView {
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
                        AccountSettingsView(appModel: appModel)
                    }
                }
            }

            // Top Incoming Call Alert Banner
            IncomingCallBanner(appModel: appModel)
                .padding(.top, 8)
        }
    }
}
