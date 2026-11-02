import SwiftUI

@main
struct SipSdkMacApp: App {
    @StateObject private var appModel = SipAppModel()

    var body: some Scene {
        WindowGroup {
            MainSplitView(appModel: appModel)
                .frame(minWidth: 750, minHeight: 550)
                .onAppear {
                    // Automatically attempt registration on launch with stored credentials
                    appModel.registerAccount()
                }
        }
        .windowStyle(.titleBar)
        .windowToolbarStyle(.unified)
        .commands {
            SidebarCommands()
        }
    }
}
