import SwiftUI

public struct AccountSettingsView: View {
    @ObservedObject var appModel: SipAppModel

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        SettingsView(appModel: appModel)
    }
}
