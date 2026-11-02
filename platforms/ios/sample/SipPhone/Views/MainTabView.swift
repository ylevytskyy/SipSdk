//
//  MainTabView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct MainTabView: View {
    @Bindable var model: SipAppModel

    var body: some View {
        TabView(selection: $model.selectedTab) {
            HomeView(model: model)
                .tabItem {
                    Label("Keypad", systemImage: "circle.grid.3x3.fill")
                }
                .tag(0)

            CallHistoryView(model: model)
                .tabItem {
                    Label("Recents", systemImage: "clock.fill")
                }
                .tag(1)

            NavigationStack {
                SettingsView(model: model)
            }
            .tabItem {
                Label("Settings", systemImage: "gearshape.fill")
            }
            .tag(2)
        }
        .sheet(isPresented: $model.isShowingAccountSheet) {
            SettingsView(model: model)
        }
        .fullScreenCover(item: Binding<SipCall?>(
            get: { model.incomingCall },
            set: { model.incomingCall = $0 }
        )) { call in
            IncomingCallView(model: model, call: call)
        }
        .fullScreenCover(isPresented: $model.isShowingInCallView) {
            if let call = model.client.activeCall {
                InCallView(model: model, call: call)
            } else {
                VStack {
                    Text("Call Ended")
                        .font(.headline)
                    Button("Dismiss") {
                        model.isShowingInCallView = false
                    }
                    .padding()
                }
            }
        }
    }
}
