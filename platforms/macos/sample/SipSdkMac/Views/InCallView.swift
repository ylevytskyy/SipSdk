import SwiftUI

public struct InCallView: View {
    @ObservedObject var appModel: SipAppModel
    @State private var showKeypad: Bool = false

    private let dtmfDigits = [
        ["1", "2", "3"],
        ["4", "5", "6"],
        ["7", "8", "9"],
        ["*", "0", "#"]
    ]

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        ZStack {
            // Background / Remote Video Surface
            if appModel.hasRemoteVideo {
                SipVideoViewRepresentable(viewType: .remote) { view, type in
                    appModel.attachVideoView(view, type: type)
                }
                .edgesIgnoringSafeArea(.all)
            } else {
                Color(NSColor.underPageBackgroundColor)
                    .edgesIgnoringSafeArea(.all)

                VStack(spacing: 16) {
                    Image(systemName: "person.crop.circle.fill")
                        .resizable()
                        .frame(width: 96, height: 96)
                        .foregroundColor(.secondary)

                    Text(appModel.activeCall?.remoteUri ?? "Unknown Peer")
                        .font(.title2)
                        .fontWeight(.semibold)

                    Text(callStateDescription)
                        .font(.headline)
                        .foregroundColor(.secondary)
                }
            }

            // Local Camera PIP Preview (Overlay in top-right corner)
            if appModel.hasRemoteVideo {
                VStack {
                    HStack {
                        Spacer()
                        SipVideoViewRepresentable(viewType: .localPreview) { view, type in
                            appModel.attachVideoView(view, type: type)
                        }
                        .frame(width: 140, height: 105)
                        .cornerRadius(10)
                        .overlay(
                            RoundedRectangle(cornerRadius: 10)
                                .stroke(Color.white.opacity(0.3), lineWidth: 1)
                        )
                        .shadow(radius: 6)
                        .padding(16)
                    }
                    Spacer()
                }
            }

            // Top Status Bar Overlay
            VStack {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(appModel.activeCall?.remoteUri ?? "Active Call")
                            .font(.headline)
                            .foregroundColor(.white)
                            .shadow(radius: 4)

                        HStack(spacing: 8) {
                            Text(callStateDescription)
                                .font(.subheadline)
                                .foregroundColor(.white.opacity(0.9))

                            Text("•")
                                .foregroundColor(.white.opacity(0.6))

                            Text(appModel.callDurationFormatted)
                                .font(.subheadline)
                                .fontWeight(.medium)
                                .foregroundColor(.white)
                        }
                        .shadow(radius: 4)
                    }

                    Spacer()
                }
                .padding()
                .background(
                    LinearGradient(colors: [Color.black.opacity(0.7), Color.clear],
                                   startPoint: .top,
                                   endPoint: .bottom)
                )

                Spacer()

                // In-Call DTMF Keypad Popover
                if showKeypad {
                    VStack(spacing: 8) {
                        ForEach(dtmfDigits, id: \.self) { row in
                            HStack(spacing: 8) {
                                ForEach(row, id: \.self) { digit in
                                    Button(action: {
                                        appModel.sendDtmf(digit)
                                    }) {
                                        Text(digit)
                                            .font(.title2)
                                            .frame(width: 48, height: 38)
                                            .background(Color.white.opacity(0.2))
                                            .foregroundColor(.white)
                                            .cornerRadius(8)
                                    }
                                    .buttonStyle(.plain)
                                }
                            }
                        }
                    }
                    .padding(12)
                    .background(Color.black.opacity(0.8))
                    .cornerRadius(12)
                    .padding(.bottom, 12)
                }

                // Bottom Control Bar
                HStack(spacing: 24) {
                    // Mute / Unmute
                    Button(action: {
                        appModel.toggleMute()
                    }) {
                        VStack(spacing: 4) {
                            Image(systemName: appModel.isMuted ? "mic.slash.fill" : "mic.fill")
                                .font(.title2)
                                .frame(width: 50, height: 50)
                                .background(appModel.isMuted ? Color.orange : Color.white.opacity(0.25))
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text(appModel.isMuted ? "Unmute" : "Mute")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                    }
                    .buttonStyle(.plain)

                    // Hold / Resume
                    Button(action: {
                        appModel.toggleHold()
                    }) {
                        VStack(spacing: 4) {
                            Image(systemName: appModel.isOnHold ? "play.fill" : "pause.fill")
                                .font(.title2)
                                .frame(width: 50, height: 50)
                                .background(appModel.isOnHold ? Color.yellow : Color.white.opacity(0.25))
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text(appModel.isOnHold ? "Resume" : "Hold")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                    }
                    .buttonStyle(.plain)

                    // DTMF Keypad Toggle
                    Button(action: {
                        withAnimation {
                            showKeypad.toggle()
                        }
                    }) {
                        VStack(spacing: 4) {
                            Image(systemName: "circle.grid.3x3.fill")
                                .font(.title2)
                                .frame(width: 50, height: 50)
                                .background(showKeypad ? Color.blue : Color.white.opacity(0.25))
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text("Keypad")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                    }
                    .buttonStyle(.plain)

                    // Hangup Call
                    Button(action: {
                        appModel.hangupActiveCall()
                    }) {
                        VStack(spacing: 4) {
                            Image(systemName: "phone.down.fill")
                                .font(.title2)
                                .frame(width: 50, height: 50)
                                .background(Color.red)
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text("End")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                    }
                    .buttonStyle(.plain)
                }
                .padding(.horizontal, 32)
                .padding(.vertical, 16)
                .background(
                    LinearGradient(colors: [Color.clear, Color.black.opacity(0.8)],
                                   startPoint: .top,
                                   endPoint: .bottom)
                )
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var callStateDescription: String {
        switch appModel.callState {
        case .incoming:
            return "Incoming Call..."
        case .ringing:
            return "Ringing..."
        case .connecting:
            return "Connecting..."
        case .established:
            return appModel.isOnHold ? "On Hold" : "Connected"
        case .hold:
            return "Call On Hold"
        case .closed:
            return "Call Ended"
        @unknown default:
            return "In Call"
        }
    }
}
