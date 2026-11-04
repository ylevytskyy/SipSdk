import SwiftUI

public struct InCallView: View {
    @ObservedObject var appModel: SipAppModel
    @State private var showKeypad: Bool = false
    @State private var showDevices: Bool = false

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
                .allowsHitTesting(false)
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
                        ZStack(alignment: .topTrailing) {
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
                            .allowsHitTesting(false)

                            // Quick Camera Switch Menu
                            Menu {
                                ForEach(appModel.availableCameras, id: \.self) { camera in
                                    Button(action: {
                                        appModel.selectCamera(name: camera)
                                    }) {
                                        if camera == appModel.selectedCamera {
                                            Label(camera, systemImage: "checkmark")
                                        } else {
                                            Text(camera)
                                        }
                                    }
                                }
                            } label: {
                                Image(systemName: "camera.rotate.fill")
                                    .font(.system(size: 11, weight: .bold))
                                    .foregroundColor(.white)
                                    .padding(5)
                                    .background(Color.black.opacity(0.6))
                                    .clipShape(Circle())
                            }
                            .menuStyle(.borderlessButton)
                            .padding(6)
                        }
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

                // In-Call Media Devices Selector HUD
                if showDevices {
                    VStack(alignment: .leading, spacing: 12) {
                        HStack {
                            Label("Audio & Video Devices", systemImage: "slider.horizontal.3")
                                .font(.system(size: 14, weight: .bold))
                                .foregroundColor(.white)
                            Spacer()
                            Button(action: {
                                withAnimation {
                                    showDevices = false
                                }
                            }) {
                                Image(systemName: "xmark.circle.fill")
                                    .foregroundColor(.white.opacity(0.7))
                                    .font(.title3)
                            }
                            .buttonStyle(.plain)
                        }

                        Divider()
                            .background(Color.white.opacity(0.25))

                        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
                            // Audio Input (Microphone)
                            GridRow {
                                HStack(spacing: 6) {
                                    Image(systemName: "mic.fill")
                                        .foregroundColor(.blue)
                                        .frame(width: 16)
                                    Text("Microphone:")
                                        .font(.system(size: 13, weight: .medium))
                                        .foregroundColor(.white.opacity(0.85))
                                }
                                .frame(width: 120, alignment: .leading)

                                Picker("", selection: $appModel.selectedAudioInput) {
                                    ForEach(appModel.availableAudioInputs, id: \.self) { dev in
                                        Text(dev).tag(dev)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedAudioInput) { newDev in
                                    appModel.selectAudioInput(name: newDev)
                                }
                            }

                            // Audio Output (Speaker)
                            GridRow {
                                HStack(spacing: 6) {
                                    Image(systemName: "speaker.wave.2.fill")
                                        .foregroundColor(.green)
                                        .frame(width: 16)
                                    Text("Speaker:")
                                        .font(.system(size: 13, weight: .medium))
                                        .foregroundColor(.white.opacity(0.85))
                                }
                                .frame(width: 120, alignment: .leading)

                                Picker("", selection: $appModel.selectedAudioOutput) {
                                    ForEach(appModel.availableAudioOutputs, id: \.self) { dev in
                                        Text(dev).tag(dev)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedAudioOutput) { newDev in
                                    appModel.selectAudioOutput(name: newDev)
                                }
                            }

                            // Video Input (Camera)
                            GridRow {
                                HStack(spacing: 6) {
                                    Image(systemName: "camera.fill")
                                        .foregroundColor(.orange)
                                        .frame(width: 16)
                                    Text("Camera:")
                                        .font(.system(size: 13, weight: .medium))
                                        .foregroundColor(.white.opacity(0.85))
                                }
                                .frame(width: 120, alignment: .leading)

                                Picker("", selection: $appModel.selectedCamera) {
                                    ForEach(appModel.availableCameras, id: \.self) { camera in
                                        Text(camera).tag(camera)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedCamera) { newCamera in
                                    appModel.selectCamera(name: newCamera)
                                }
                            }

                            // Video Output (Display)
                            GridRow {
                                HStack(spacing: 6) {
                                    Image(systemName: "display")
                                        .foregroundColor(.purple)
                                        .frame(width: 16)
                                    Text("Display:")
                                        .font(.system(size: 13, weight: .medium))
                                        .foregroundColor(.white.opacity(0.85))
                                }
                                .frame(width: 120, alignment: .leading)

                                Picker("", selection: $appModel.selectedVideoOutput) {
                                    ForEach(appModel.availableVideoOutputs, id: \.self) { screen in
                                        Text(screen).tag(screen)
                                    }
                                }
                                .pickerStyle(.menu)
                                .onChange(of: appModel.selectedVideoOutput) { newScreen in
                                    appModel.selectVideoOutput(name: newScreen)
                                }
                            }
                        }
                    }
                    .padding(14)
                    .background(
                        RoundedRectangle(cornerRadius: 14)
                            .fill(Color.black.opacity(0.85))
                            .overlay(
                                RoundedRectangle(cornerRadius: 14)
                                    .stroke(Color.white.opacity(0.2), lineWidth: 1)
                            )
                            .shadow(radius: 10)
                    )
                    .padding(.bottom, 12)
                }

                // Bottom Control Bar
                HStack(spacing: 20) {
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
                        .frame(minWidth: 54, minHeight: 64)
                        .contentShape(Rectangle())
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
                        .frame(minWidth: 54, minHeight: 64)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)

                    // In-Call Media Device Selection Toggle
                    Button(action: {
                        withAnimation {
                            showDevices.toggle()
                            if showDevices {
                                showKeypad = false
                                appModel.refreshMediaDevices()
                            }
                        }
                    }) {
                        VStack(spacing: 4) {
                            Image(systemName: "slider.horizontal.3")
                                .font(.title2)
                                .frame(width: 50, height: 50)
                                .background(showDevices ? Color.blue : Color.white.opacity(0.25))
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text("Devices")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                        .frame(minWidth: 54, minHeight: 64)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)

                    // DTMF Keypad Toggle
                    Button(action: {
                        withAnimation {
                            showKeypad.toggle()
                            if showKeypad {
                                showDevices = false
                            }
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
                        .frame(minWidth: 54, minHeight: 64)
                        .contentShape(Rectangle())
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
                                .background(appModel.isHangingUp ? Color.gray : Color.red)
                                .foregroundColor(.white)
                                .clipShape(Circle())
                            Text(appModel.isHangingUp ? "Ending..." : "End")
                                .font(.caption)
                                .foregroundColor(.white)
                        }
                        .frame(minWidth: 54, minHeight: 64)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .disabled(appModel.isHangingUp)
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
        .onAppear {
            appModel.refreshMediaDevices()
        }
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
