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
        .overlay(alignment: .top) {
            if let message = model.busyBannerMessage {
                Text(message)
                    .font(.caption.weight(.semibold))
                    .foregroundColor(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(Color.red.opacity(0.9))
                    .clipShape(Capsule())
                    .padding(.top, 8)
            }
        }
        .sheet(isPresented: $model.isShowingAccountSheet) {
            SettingsView(model: model)
        }
        .fullScreenCover(item: Binding<SipCall?>(
            get: { model.isShowingInCallView ? nil : model.incomingCall },
            set: { model.incomingCall = $0 }
        )) { call in
            IncomingCallView(model: model, call: call)
        }
        .fullScreenCover(isPresented: $model.isShowingInCallView) {
            if let call = model.focusedCall {
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
