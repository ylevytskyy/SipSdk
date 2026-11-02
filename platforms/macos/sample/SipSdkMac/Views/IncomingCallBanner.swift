import SwiftUI

public struct IncomingCallBanner: View {
    @ObservedObject var appModel: SipAppModel

    public init(appModel: SipAppModel) {
        self.appModel = appModel
    }

    public var body: some View {
        if let call = appModel.incomingCall {
            HStack(spacing: 16) {
                Image(systemName: call.hasVideo ? "video.circle.fill" : "phone.circle.fill")
                    .resizable()
                    .frame(width: 36, height: 36)
                    .foregroundColor(.accentColor)

                VStack(alignment: .leading, spacing: 2) {
                    Text("Incoming SIP Call")
                        .font(.headline)
                        .foregroundColor(.primary)
                    Text(call.remoteUri)
                        .font(.subheadline)
                        .foregroundColor(.secondary)
                }

                Spacer()

                Button(action: {
                    appModel.declineIncomingCall()
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "phone.down.fill")
                        Text("Decline")
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)

                Button(action: {
                    appModel.answerIncomingCall(withVideo: false)
                }) {
                    HStack(spacing: 4) {
                        Image(systemName: "phone.fill")
                        Text("Audio")
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                }
                .buttonStyle(.borderedProminent)
                .tint(.green)

                if call.hasVideo {
                    Button(action: {
                        appModel.answerIncomingCall(withVideo: true)
                    }) {
                        HStack(spacing: 4) {
                            Image(systemName: "video.fill")
                            Text("Video")
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.blue)
                }
            }
            .padding()
            .background(Color(NSColor.windowBackgroundColor))
            .cornerRadius(12)
            .shadow(color: Color.black.opacity(0.15), radius: 8, x: 0, y: 4)
            .padding(.horizontal)
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }
}
