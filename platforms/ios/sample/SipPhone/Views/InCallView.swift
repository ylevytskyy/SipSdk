//
//  InCallView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct InCallView: View {
    @Bindable var model: SipAppModel
    let call: SipCall

    @State private var remoteVideoUIView = SipVideoUIView()
    @State private var localVideoUIView = SipVideoUIView()
    @State private var timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    @State private var isShowingKeypad = false

    var body: some View {
        ZStack {
            // Background / Remote Video
            Color.black.ignoresSafeArea()

            if call.hasVideo {
                SipVideoView(videoUIView: remoteVideoUIView)
                    .ignoresSafeArea()
                    .onAppear {
                        model.client.remoteVideoView = remoteVideoUIView
                    }
            } else {
                VStack(spacing: 16) {
                    Image(systemName: "person.crop.circle.fill")
                        .font(.system(size: 96))
                        .foregroundColor(.gray)

                    Text(call.peerDisplayName ?? call.remoteUri)
                        .font(.system(size: 24, weight: .bold))
                        .foregroundColor(.white)
                }
            }

            // Top Header: Remote party & duration
            VStack {
                topHeader
                Spacer()
            }
            .padding(.top, 16)

            // Hold indicator overlay
            if call.isOnHold {
                ZStack {
                    Color.black.opacity(0.6).ignoresSafeArea()
                    VStack(spacing: 12) {
                        Image(systemName: "pause.circle.fill")
                            .font(.system(size: 56))
                            .foregroundColor(.yellow)
                        Text("Call on Hold")
                            .font(.system(size: 20, weight: .bold))
                            .foregroundColor(.white)
                    }
                }
            }

            // PIP Local Preview (bottom right)
            if model.client.isLocalVideoEnabled {
                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        pipLocalPreview
                            .padding(.trailing, 20)
                            .padding(.bottom, 120)
                    }
                }
            }

            // Bottom Controls Toolbar
            VStack {
                Spacer()
                controlsToolbar
            }
            .padding(.bottom, 30)
        }
        .sheet(isPresented: $isShowingKeypad) {
            KeypadSheet(call: call)
                .presentationDetents([.medium])
        }
        .onReceive(timer) { _ in
            call.updateDuration()
        }
    }

    // MARK: - Subviews

    private var topHeader: some View {
        VStack(spacing: 4) {
            Text(call.peerDisplayName ?? call.remoteUri)
                .font(.system(size: 20, weight: .bold))
                .foregroundColor(.white)
                .shadow(radius: 4)

            Text(headerStatusText)
                .font(.system(size: 14, weight: .medium))
                .foregroundColor(.white.opacity(0.8))
                .shadow(radius: 4)
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 10)
        .background(Color.black.opacity(0.3))
        .cornerRadius(20)
    }

    private var headerStatusText: String {
        switch call.state {
        case .incoming: return "Incoming Call..."
        case .ringing: return "Ringing..."
        case .connecting: return "Connecting..."
        case .established:
            let m = call.durationSeconds / 60
            let s = call.durationSeconds % 60
            return String(format: "%02d:%02d", m, s)
        case .hold: return "On Hold"
        case .closed(let code, let reason): return "Ended (\(code): \(reason))"
        }
    }

    private var pipLocalPreview: some View {
        ZStack(alignment: .topTrailing) {
            SipVideoView(videoUIView: localVideoUIView)
                .frame(width: 100, height: 140)
                .cornerRadius(12)
                .overlay(
                    RoundedRectangle(cornerRadius: 12)
                        .stroke(Color.white.opacity(0.6), lineWidth: 1.5)
                )
                .shadow(radius: 8)
                .onAppear {
                    model.client.localVideoView = localVideoUIView
                }

            Button {
                model.client.switchCamera()
            } label: {
                Image(systemName: "camera.rotate.fill")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundColor(.white)
                    .padding(6)
                    .background(Color.black.opacity(0.5))
                    .clipShape(Circle())
            }
            .padding(6)
        }
    }

    private var controlsToolbar: some View {
        HStack(spacing: 20) {
            // Mute Button
            controlButton(
                icon: model.client.isMicrophoneMuted ? "mic.slash.fill" : "mic.fill",
                label: "Mute",
                isActive: model.client.isMicrophoneMuted
            ) {
                model.client.isMicrophoneMuted.toggle()
            }

            // Video Toggle Button
            controlButton(
                icon: model.client.isLocalVideoEnabled ? "video.fill" : "video.slash.fill",
                label: "Video",
                isActive: !model.client.isLocalVideoEnabled
            ) {
                model.client.isLocalVideoEnabled.toggle()
            }

            // Keypad Button
            controlButton(
                icon: "circle.grid.3x3.fill",
                label: "Keypad",
                isActive: false
            ) {
                isShowingKeypad = true
            }

            // Speaker Button
            controlButton(
                icon: model.client.isSpeakerEnabled ? "speaker.wave.3.fill" : "speaker.fill",
                label: "Speaker",
                isActive: model.client.isSpeakerEnabled
            ) {
                model.client.isSpeakerEnabled.toggle()
            }

            // Hold Button
            controlButton(
                icon: call.isOnHold ? "play.fill" : "pause.fill",
                label: call.isOnHold ? "Resume" : "Hold",
                isActive: call.isOnHold
            ) {
                Task {
                    try? await call.setHold(!call.isOnHold)
                }
            }

            // Hangup Button
            Button {
                Task {
                    await call.hangup()
                    model.isShowingInCallView = false
                }
            } label: {
                Image(systemName: "phone.down.fill")
                    .font(.system(size: 24, weight: .bold))
                    .foregroundColor(.white)
                    .frame(width: 56, height: 56)
                    .background(Color.red)
                    .clipShape(Circle())
                    .shadow(radius: 6)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
        .background(Color.black.opacity(0.45))
        .cornerRadius(32)
        .padding(.horizontal, 16)
    }

    private func controlButton(icon: String, label: String, isActive: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 4) {
                Image(systemName: icon)
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundColor(isActive ? .black : .white)
                    .frame(width: 46, height: 46)
                    .background(isActive ? Color.white : Color.white.opacity(0.2))
                    .clipShape(Circle())
            }
        }
    }
}

// MARK: - In-Call DTMF Keypad Sheet

struct KeypadSheet: View {
    let call: SipCall
    @Environment(\.dismiss) private var dismiss

    let keys = [
        ["1", "2", "3"],
        ["4", "5", "6"],
        ["7", "8", "9"],
        ["*", "0", "#"]
    ]

    var body: some View {
        VStack(spacing: 16) {
            HStack {
                Text("Dialpad (DTMF)")
                    .font(.system(size: 18, weight: .bold))
                Spacer()
                Button("Done") {
                    dismiss()
                }
            }
            .padding(.horizontal, 24)
            .padding(.top, 16)

            VStack(spacing: 12) {
                ForEach(keys, id: \.self) { row in
                    HStack(spacing: 24) {
                        ForEach(row, id: \.self) { digit in
                            Button {
                                try? call.sendDTMF(digit)
                            } label: {
                                Text(digit)
                                    .font(.system(size: 28, weight: .semibold))
                                    .foregroundColor(.primary)
                                    .frame(width: 68, height: 68)
                                    .background(Color(uiColor: .secondarySystemFill))
                                    .clipShape(Circle())
                            }
                        }
                    }
                }
            }
            Spacer()
        }
    }
}
