//
//  SipAppModel.swift
//  SipPhone (iOS Sample App)
//

import SwiftUI
import SipSdk

@MainActor
@Observable
public final class SipAppModel {
    public let client: SipClient
    public let accountStore: AccountStore

    public var history: [CallHistoryItem] = []
    public var selectedTab: Int = 0
    public var dialDestination: String = "1002"

    public var incomingCall: SipCall?
    public var isShowingInCallView: Bool = false
    public var isShowingAccountSheet: Bool = false
    public var isShowingKeypadSheet: Bool = false
    public var busyBannerMessage: String?
    public var selectedCallId: UUID?

    private let historyKey = "com.sipsdk.sample.history"

    public var openCalls: [SipCall] {
        client.calls.filter { !$0.state.isClosed }
    }

    public var focusedCall: SipCall? {
        if let id = selectedCallId, let match = openCalls.first(where: { $0.id == id }) {
            return match
        }
        return client.activeCall ?? openCalls.first
    }

    public func showBusyBanner(_ message: String) {
        busyBannerMessage = message
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 4_000_000_000)
            if self?.busyBannerMessage == message {
                self?.busyBannerMessage = nil
            }
        }
    }

    public func swapToCall(_ call: SipCall) async {
        selectedCallId = call.id
        if call.isOnHold {
            try? await call.setHold(false)
        }
    }

    public init() {
        self.client = SipClient()
        self.accountStore = AccountStore()
        loadHistory()
        setupEventObserver()
    }

    public func startAndRegister() async {
        do {
            if !client.isStarted {
                try await client.start()
            }
            try await client.register(accountStore.account)
        } catch {
            NSLog("[SipPhone] Failed to start and register: %@", error.localizedDescription)
        }
    }

    public func makeCall(to destination: String, withVideo: Bool = true) async {
        guard !destination.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        do {
            if !client.isStarted {
                try await client.start()
            }
            let call = try await client.makeCall(to: destination, withVideo: withVideo)
            selectedCallId = call.id
            isShowingInCallView = true

            // Record to history
            let item = CallHistoryItem(
                remoteUri: call.remoteUri,
                displayName: call.peerDisplayName,
                date: Date(),
                durationSeconds: 0,
                direction: .outgoing,
                hasVideo: withVideo
            )
            history.insert(item, at: 0)
            saveHistory()
        } catch SipError.busy {
            showBusyBanner("Maximum concurrent calls reached (Busy)")
        } catch {
            NSLog("[SipPhone] Failed to place call: %@", error.localizedDescription)
        }
    }

    public func answerIncomingCall(withVideo: Bool) async {
        guard let call = incomingCall else { return }
        incomingCall = nil
        do {
            try await call.answer(withVideo: withVideo)
            selectedCallId = call.id
            isShowingInCallView = true
        } catch {
            NSLog("[SipPhone] Failed to answer incoming call: %@", error.localizedDescription)
        }
    }

    public func declineIncomingCall() async {
        guard let call = incomingCall else { return }
        incomingCall = nil
        await call.hangup(statusCode: 603, reason: "Decline")
    }

    public func clearHistory() {
        history.removeAll()
        saveHistory()
    }

    private func setupEventObserver() {
        Task { [weak self] in
            guard let self = self else { return }
            for await event in self.client.events {
                switch event {
                case .incomingCall(let call):
                    self.incomingCall = call
                    let item = CallHistoryItem(
                        remoteUri: call.remoteUri,
                        displayName: call.peerDisplayName,
                        date: Date(),
                        durationSeconds: 0,
                        direction: .incoming,
                        hasVideo: call.hasVideo
                    )
                    self.history.insert(item, at: 0)
                    self.saveHistory()

                case .callStateChanged(let call, let state):
                    if state.isConnected {
                        self.selectedCallId = call.id
                        self.isShowingInCallView = true
                    } else if state.isClosed {
                        if self.incomingCall?.id == call.id {
                            self.incomingCall = nil
                        }
                        if self.selectedCallId == call.id {
                            self.selectedCallId = self.openCalls.first?.id
                        }
                        if self.openCalls.isEmpty {
                            self.selectedCallId = nil
                            self.isShowingInCallView = false
                        }
                        // Update last history item duration
                        if let idx = self.history.firstIndex(where: { $0.remoteUri == call.remoteUri }) {
                            var updated = self.history[idx]
                            let duration = call.durationSeconds
                            self.history[idx] = CallHistoryItem(
                                id: updated.id,
                                remoteUri: updated.remoteUri,
                                displayName: updated.displayName,
                                date: updated.date,
                                durationSeconds: max(updated.durationSeconds, duration),
                                direction: updated.direction,
                                hasVideo: updated.hasVideo
                            )
                            self.saveHistory()
                        }
                    }

                case .callRejectedBusy(let callerUri):
                    self.showBusyBanner("Line busy — rejected call from \(callerUri)")

                default:
                    break
                }
            }
        }
    }

    private func loadHistory() {
        if let data = UserDefaults.standard.data(forKey: historyKey),
           let saved = try? JSONDecoder().decode([CallHistoryItem].self, from: data) {
            self.history = saved
        }
    }

    private func saveHistory() {
        if let data = try? JSONEncoder().encode(history) {
            UserDefaults.standard.set(data, forKey: historyKey)
        }
    }
}
