//
//  SipPhoneApp.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

@main
struct SipPhoneApp: App {
    @State private var model = SipAppModel()

    var body: some Scene {
        WindowGroup {
            MainTabView(model: model)
                .task {
                    await model.startAndRegister()
                }
        }
    }
}
