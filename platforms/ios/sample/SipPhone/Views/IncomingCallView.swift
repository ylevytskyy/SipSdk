//
//  IncomingCallView.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

struct IncomingCallView: View {
    @Bindable var model: SipAppModel
    let call: SipCall

    @State private var pulseScale: CGFloat = 1.0

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            VStack(spacing: 32) {
                Spacer()

                // Caller identity
                VStack(spacing: 12) {
                    ZStack {
                        Circle()
                            .fill(Color.blue.opacity(0.2))
                            .frame(width: 130, height: 130)
                            .scaleEffect(pulseScale)
                            .animation(
                                .easeInOut(duration: 1.2).repeatForever(autoreverses: true),
                                value: pulseScale
                            )

                        Circle()
                            .fill(Color.blue.opacity(0.5))
                            .frame(width: 100, height: 100)

                        Image(systemName: "person.crop.circle.fill")
                            .font(.system(size: 72))
                            .foregroundColor(.white)
                    }
                    .padding(.bottom, 12)

                    Text(call.peerDisplayName ?? call.remoteUri)
                        .font(.system(size: 28, weight: .bold))
                        .foregroundColor(.white)

                    Text(call.remoteUri)
                        .font(.system(size: 16))
                        .foregroundColor(.gray)

                    Text(call.hasVideo ? "Incoming SIP Video Call" : "Incoming SIP Audio Call")
                        .font(.system(size: 14, weight: .medium))
                        .foregroundColor(.secondary)
                        .padding(.top, 4)
                }

                Spacer()

                // Actions: Decline, Answer Audio, Answer Video
                HStack(spacing: 40) {
                    // Decline button
                    VStack(spacing: 8) {
                        Button {
                            Task {
                                await model.declineIncomingCall()
                            }
                        } label: {
                            Image(systemName: "phone.down.fill")
                                .font(.system(size: 26))
                                .foregroundColor(.white)
                                .frame(width: 68, height: 68)
                                .background(Color.red)
                                .clipShape(Circle())
                        }
                        Text("Decline")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(.white)
                    }

                    // Answer Audio
                    VStack(spacing: 8) {
                        Button {
                            Task {
                                await model.answerIncomingCall(withVideo: false)
                            }
                        } label: {
                            Image(systemName: "phone.fill")
                                .font(.system(size: 26))
                                .foregroundColor(.white)
                                .frame(width: 68, height: 68)
                                .background(Color.green)
                                .clipShape(Circle())
                        }
                        Text("Audio")
                            .font(.system(size: 12, weight: .medium))
                            .foregroundColor(.white)
                    }

                    // Answer Video
                    if call.hasVideo {
                        VStack(spacing: 8) {
                            Button {
                                Task {
                                    await model.answerIncomingCall(withVideo: true)
                                }
                            } label: {
                                Image(systemName: "video.fill")
                                    .font(.system(size: 26))
                                    .foregroundColor(.white)
                                    .frame(width: 68, height: 68)
                                    .background(Color.blue)
                                    .clipShape(Circle())
                            }
                            Text("Video")
                                .font(.system(size: 12, weight: .medium))
                                .foregroundColor(.white)
                        }
                    }
                }
                .padding(.bottom, 48)
            }
            .padding()
        }
        .onAppear {
            pulseScale = 1.15
        }
    }
}
