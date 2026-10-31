//
//  SipCallDetailsViewController.swift
//  SipSdk
//
//  Created by Yuriy Levytskyy on 2/28/19.
//  Copyright © 2019 Yuriy Levytskyy. All rights reserved.
//

import UIKit

class SipCallDetailsViewController: UIViewController {
    var sipCall: SipCall!
    weak var sipClient: SipClient?
    var isSimulatedVideoPreview: Bool = false
    var onCallEnded: (() -> Void)?

    private let remoteVideoView = SipVideoView(frame: .zero)
    private let localVideoView = SipVideoView(frame: .zero)

    private let headerOverlay = UIView()
    private let remoteUriTitleLabel = UILabel()
    private let callStateSubtitleLabel = UILabel()
    private let videoResolutionBadge = UILabel()

    private let dtmfContainerView = UIView()
    private let dtmfSequenceLabel = UILabel()

    private let controlsContainerView = UIView()
    private var muteButton: UIButton!
    private var videoToggleButton: UIButton!
    private var flipCameraButton: UIButton!
    private var speakerButton: UIButton!
    private var holdButton: UIButton!
    private var keypadButton: UIButton!
    private var endCallButton: UIButton!

    private var durationTimer: Timer?
    private var syntheticFrameTimer: Timer?
    private var elapsedSeconds: Int = 0
    private var enteredDtmfDigits: String = ""

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.05, green: 0.07, blue: 0.11, alpha: 1.0)
        title = "In-Call Video"

        buildVideoCanvasHierarchy()
        buildHeaderOverlay()
        buildDtmfKeypadOverlay()
        buildControlsBar()
        attachVideoViewsToClient()
        refreshControlStates()
        startCallTimers()
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        durationTimer?.invalidate()
        durationTimer = nil
        syntheticFrameTimer?.invalidate()
        syntheticFrameTimer = nil
    }

    // MARK: - Storyboard IBActions (Preserved I8)

    @IBAction func hold(_ sender: Any) {
        guard let call = sipCall else { return }
        let targetHold = !call.isOnHold
        _ = call.setHold(targetHold)
        SipCallKitManager.shared.requestHold(call, onHold: targetHold)
        refreshControlStates()
    }

    @IBAction func hangup(_ sender: Any) {
        if let call = sipCall {
            call.hangup(487, reason: "Request Terminated")
            SipCallKitManager.shared.reportCallEnded(call, reason: .remoteEnded)
        }
        onCallEnded?()
        if let nav = navigationController {
            nav.popViewController(animated: true)
        } else {
            dismiss(animated: true)
        }
    }

    // MARK: - Programmatic UI Setup

    private func buildVideoCanvasHierarchy() {
        // Hide the legacy storyboard bottom stack if present so the full-bleed video canvas takes over cleanly
        for subview in view.subviews {
            if subview is UIStackView {
                subview.isHidden = true
            }
        }

        remoteVideoView.translatesAutoresizingMaskIntoConstraints = false
        remoteVideoView.accessibilityIdentifier = "remoteVideoView"
        view.addSubview(remoteVideoView)

        localVideoView.translatesAutoresizingMaskIntoConstraints = false
        localVideoView.accessibilityIdentifier = "localVideoView"
        localVideoView.layer.cornerRadius = 12
        localVideoView.layer.borderWidth = 2
        localVideoView.layer.borderColor = UIColor.white.withAlphaComponent(0.85).cgColor
        localVideoView.clipsToBounds = true
        view.addSubview(localVideoView)

        let panGesture = UIPanGestureRecognizer(target: self, action: #selector(handleLocalPreviewPan(_:)))
        localVideoView.addGestureRecognizer(panGesture)
        localVideoView.isUserInteractionEnabled = true

        NSLayoutConstraint.activate([
            remoteVideoView.topAnchor.constraint(equalTo: view.topAnchor),
            remoteVideoView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            remoteVideoView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            remoteVideoView.bottomAnchor.constraint(equalTo: view.bottomAnchor),

            localVideoView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 88),
            localVideoView.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -16),
            localVideoView.widthAnchor.constraint(equalToConstant: 116),
            localVideoView.heightAnchor.constraint(equalToConstant: 158)
        ])
    }

    private func buildHeaderOverlay() {
        headerOverlay.translatesAutoresizingMaskIntoConstraints = false
        headerOverlay.backgroundColor = UIColor.black.withAlphaComponent(0.55)
        headerOverlay.layer.cornerRadius = 14
        view.addSubview(headerOverlay)

        remoteUriTitleLabel.translatesAutoresizingMaskIntoConstraints = false
        remoteUriTitleLabel.font = UIFont.systemFont(ofSize: 17, weight: .bold)
        remoteUriTitleLabel.textColor = .white
        remoteUriTitleLabel.textAlignment = .center
        remoteUriTitleLabel.text = sipCall?.remoteUri ?? "sip:video-peer@iptel.org"

        callStateSubtitleLabel.translatesAutoresizingMaskIntoConstraints = false
        callStateSubtitleLabel.font = UIFont.monospacedDigitSystemFont(ofSize: 13, weight: .medium)
        callStateSubtitleLabel.textColor = UIColor(red: 0.55, green: 0.94, blue: 0.68, alpha: 1.0)
        callStateSubtitleLabel.textAlignment = .center
        callStateSubtitleLabel.text = "00:00 • HD H.264 Video"

        videoResolutionBadge.translatesAutoresizingMaskIntoConstraints = false
        videoResolutionBadge.font = UIFont.systemFont(ofSize: 11, weight: .semibold)
        videoResolutionBadge.textColor = .white
        videoResolutionBadge.backgroundColor = UIColor.systemBlue.withAlphaComponent(0.85)
        videoResolutionBadge.layer.cornerRadius = 6
        videoResolutionBadge.layer.masksToBounds = true
        videoResolutionBadge.textAlignment = .center
        videoResolutionBadge.text = " H.264 640×480 "

        headerOverlay.addSubview(remoteUriTitleLabel)
        headerOverlay.addSubview(callStateSubtitleLabel)
        headerOverlay.addSubview(videoResolutionBadge)

        NSLayoutConstraint.activate([
            headerOverlay.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 8),
            headerOverlay.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 16),
            headerOverlay.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -16),

            remoteUriTitleLabel.topAnchor.constraint(equalTo: headerOverlay.topAnchor, constant: 10),
            remoteUriTitleLabel.leadingAnchor.constraint(equalTo: headerOverlay.leadingAnchor, constant: 12),
            remoteUriTitleLabel.trailingAnchor.constraint(equalTo: headerOverlay.trailingAnchor, constant: -12),

            callStateSubtitleLabel.topAnchor.constraint(equalTo: remoteUriTitleLabel.bottomAnchor, constant: 4),
            callStateSubtitleLabel.leadingAnchor.constraint(equalTo: headerOverlay.leadingAnchor, constant: 12),
            callStateSubtitleLabel.trailingAnchor.constraint(equalTo: headerOverlay.trailingAnchor, constant: -12),

            videoResolutionBadge.topAnchor.constraint(equalTo: callStateSubtitleLabel.bottomAnchor, constant: 6),
            videoResolutionBadge.centerXAnchor.constraint(equalTo: headerOverlay.centerXAnchor),
            videoResolutionBadge.bottomAnchor.constraint(equalTo: headerOverlay.bottomAnchor, constant: -10),
            videoResolutionBadge.heightAnchor.constraint(equalToConstant: 20)
        ])
    }

    private func buildDtmfKeypadOverlay() {
        dtmfContainerView.translatesAutoresizingMaskIntoConstraints = false
        dtmfContainerView.backgroundColor = UIColor.black.withAlphaComponent(0.82)
        dtmfContainerView.layer.cornerRadius = 16
        dtmfContainerView.isHidden = true
        view.addSubview(dtmfContainerView)

        dtmfSequenceLabel.translatesAutoresizingMaskIntoConstraints = false
        dtmfSequenceLabel.font = UIFont.monospacedDigitSystemFont(ofSize: 18, weight: .bold)
        dtmfSequenceLabel.textColor = .white
        dtmfSequenceLabel.textAlignment = .center
        dtmfSequenceLabel.text = "DTMF Keypad"
        dtmfContainerView.addSubview(dtmfSequenceLabel)

        let rows = [
            ["1", "2", "3"],
            ["4", "5", "6"],
            ["7", "8", "9"],
            ["*", "0", "#"]
        ]
        let gridStack = UIStackView()
        gridStack.axis = .vertical
        gridStack.spacing = 8
        gridStack.distribution = .fillEqually
        gridStack.translatesAutoresizingMaskIntoConstraints = false

        for row in rows {
            let rowStack = UIStackView()
            rowStack.axis = .horizontal
            rowStack.spacing = 10
            rowStack.distribution = .fillEqually
            for digit in row {
                let btn = UIButton(type: .system)
                btn.setTitle(digit, for: .normal)
                btn.titleLabel?.font = UIFont.systemFont(ofSize: 20, weight: .semibold)
                btn.setTitleColor(.white, for: .normal)
                btn.backgroundColor = UIColor.white.withAlphaComponent(0.18)
                btn.layer.cornerRadius = 10
                btn.addTarget(self, action: #selector(handleDtmfButton(_:)), for: .touchUpInside)
                rowStack.addArrangedSubview(btn)
            }
            gridStack.addArrangedSubview(rowStack)
        }

        dtmfContainerView.addSubview(gridStack)

        NSLayoutConstraint.activate([
            dtmfContainerView.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            dtmfContainerView.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -150),
            dtmfContainerView.widthAnchor.constraint(equalToConstant: 240),
            dtmfContainerView.heightAnchor.constraint(equalToConstant: 230),

            dtmfSequenceLabel.topAnchor.constraint(equalTo: dtmfContainerView.topAnchor, constant: 10),
            dtmfSequenceLabel.leadingAnchor.constraint(equalTo: dtmfContainerView.leadingAnchor, constant: 10),
            dtmfSequenceLabel.trailingAnchor.constraint(equalTo: dtmfContainerView.trailingAnchor, constant: -10),

            gridStack.topAnchor.constraint(equalTo: dtmfSequenceLabel.bottomAnchor, constant: 8),
            gridStack.leadingAnchor.constraint(equalTo: dtmfContainerView.leadingAnchor, constant: 14),
            gridStack.trailingAnchor.constraint(equalTo: dtmfContainerView.trailingAnchor, constant: -14),
            gridStack.bottomAnchor.constraint(equalTo: dtmfContainerView.bottomAnchor, constant: -12)
        ])
    }

    private func buildControlsBar() {
        controlsContainerView.translatesAutoresizingMaskIntoConstraints = false
        controlsContainerView.backgroundColor = UIColor.black.withAlphaComponent(0.68)
        controlsContainerView.layer.cornerRadius = 20
        view.addSubview(controlsContainerView)

        muteButton = makeControlButton(title: "Mute", action: #selector(toggleMuteTapped))
        videoToggleButton = makeControlButton(title: "Video On", action: #selector(toggleVideoTapped))
        flipCameraButton = makeControlButton(title: "Flip Cam", action: #selector(flipCameraTapped))
        speakerButton = makeControlButton(title: "Speaker", action: #selector(toggleSpeakerTapped))
        holdButton = makeControlButton(title: "Hold", action: #selector(hold(_:)))
        keypadButton = makeControlButton(title: "Keypad", action: #selector(toggleKeypadTapped))

        endCallButton = UIButton(type: .system)
        endCallButton.setTitle("End Call", for: .normal)
        endCallButton.titleLabel?.font = UIFont.systemFont(ofSize: 15, weight: .bold)
        endCallButton.setTitleColor(.white, for: .normal)
        endCallButton.backgroundColor = .systemRed
        endCallButton.layer.cornerRadius = 12
        endCallButton.addTarget(self, action: #selector(hangup(_:)), for: .touchUpInside)

        let topRow = UIStackView(arrangedSubviews: [muteButton, videoToggleButton, flipCameraButton, speakerButton])
        topRow.axis = .horizontal
        topRow.spacing = 8
        topRow.distribution = .fillEqually

        let bottomRow = UIStackView(arrangedSubviews: [holdButton, keypadButton, endCallButton])
        bottomRow.axis = .horizontal
        bottomRow.spacing = 8
        bottomRow.distribution = .fillEqually

        let mainStack = UIStackView(arrangedSubviews: [topRow, bottomRow])
        mainStack.axis = .vertical
        mainStack.spacing = 8
        mainStack.distribution = .fillEqually
        mainStack.translatesAutoresizingMaskIntoConstraints = false

        controlsContainerView.addSubview(mainStack)

        NSLayoutConstraint.activate([
            controlsContainerView.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 12),
            controlsContainerView.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -12),
            controlsContainerView.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -12),
            controlsContainerView.heightAnchor.constraint(equalToConstant: 112),

            mainStack.topAnchor.constraint(equalTo: controlsContainerView.topAnchor, constant: 10),
            mainStack.leadingAnchor.constraint(equalTo: controlsContainerView.leadingAnchor, constant: 10),
            mainStack.trailingAnchor.constraint(equalTo: controlsContainerView.trailingAnchor, constant: -10),
            mainStack.bottomAnchor.constraint(equalTo: controlsContainerView.bottomAnchor, constant: -10)
        ])
    }

    private func makeControlButton(title: String, action: Selector) -> UIButton {
        let btn = UIButton(type: .system)
        btn.setTitle(title, for: .normal)
        btn.titleLabel?.font = UIFont.systemFont(ofSize: 13, weight: .semibold)
        btn.setTitleColor(.white, for: .normal)
        btn.backgroundColor = UIColor.white.withAlphaComponent(0.18)
        btn.layer.cornerRadius = 10
        btn.addTarget(self, action: action, for: .touchUpInside)
        return btn
    }

    // MARK: - Video & Telephony Actions

    private func attachVideoViewsToClient() {
        sipClient?.remoteVideoView = remoteVideoView
        sipClient?.localVideoView = localVideoView

        // Render initial frames immediately so local PiP and remote canvas are live
        remoteVideoView.renderTestPattern(withWidth: 640, height: 480)
        localVideoView.renderTestPattern(withWidth: 320, height: 240)
    }

    private func startCallTimers() {
        durationTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            self.elapsedSeconds += 1
            self.updateHeaderSubtitle()
        }

        if isSimulatedVideoPreview {
            syntheticFrameTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
                guard let self = self else { return }
                self.remoteVideoView.renderTestPattern(withWidth: 640, height: 480)
                if self.sipClient?.isLocalVideoEnabled ?? true {
                    self.localVideoView.renderTestPattern(withWidth: 320, height: 240)
                }
            }
        }
    }

    private func updateHeaderSubtitle() {
        let secs = Int(sipCall?.durationSeconds ?? 0) > 0 ? Int(sipCall?.durationSeconds ?? 0) : elapsedSeconds
        let mins = secs / 60
        let rem = secs % 60
        let holdTag = (sipCall?.isOnHold == true) ? " • ON HOLD" : ""
        let camTag = (sipClient?.cameraPosition == .back) ? "Back Cam" : "Front Cam"
        callStateSubtitleLabel.text = String(format: "%02d:%02d • H.264 • %@%@", mins, rem, camTag, holdTag)
        let rSize = remoteVideoView.videoSize
        if rSize.width > 0 && rSize.height > 0 {
            videoResolutionBadge.text = " H.264 \(Int(rSize.width))×\(Int(rSize.height)) "
        }
    }

    @objc private func toggleMuteTapped() {
        guard let client = sipClient else { return }
        let nextMute = !client.isMicrophoneMuted
        client.setMuted(nextMute)
        if let call = sipCall {
            SipCallKitManager.shared.requestMute(call, muted: nextMute)
        }
        refreshControlStates()
    }

    @objc private func toggleVideoTapped() {
        guard let client = sipClient else { return }
        let nextVideo = !client.isLocalVideoEnabled
        client.isLocalVideoEnabled = nextVideo
        if let call = sipCall {
            _ = call.setVideoEnabled(nextVideo)
        }
        if !nextVideo {
            localVideoView.clearVideo()
        } else {
            localVideoView.renderTestPattern(withWidth: 320, height: 240)
        }
        refreshControlStates()
    }

    @objc private func flipCameraTapped() {
        sipClient?.switchCamera()
        localVideoView.renderTestPattern(withWidth: 320, height: 240)
        refreshControlStates()
        updateHeaderSubtitle()
    }

    @objc private func toggleSpeakerTapped() {
        guard let client = sipClient else { return }
        client.setSpeakerphone(!client.isSpeakerEnabled)
        refreshControlStates()
    }

    @objc private func toggleKeypadTapped() {
        dtmfContainerView.isHidden.toggle()
        refreshControlStates()
    }

    @objc private func handleDtmfButton(_ sender: UIButton) {
        guard let digit = sender.title(for: .normal) else { return }
        enteredDtmfDigits.append(digit)
        dtmfSequenceLabel.text = "DTMF: \(enteredDtmfDigits)"
        _ = sipCall?.sendDtmf(digit)
    }

    @objc private func handleLocalPreviewPan(_ recognizer: UIPanGestureRecognizer) {
        let translation = recognizer.translation(in: view)
        if let targetView = recognizer.view {
            targetView.center = CGPoint(x: targetView.center.x + translation.x,
                                        y: targetView.center.y + translation.y)
        }
        recognizer.setTranslation(.zero, in: view)
    }

    func refreshControlStates() {
        let muted = sipClient?.isMicrophoneMuted ?? false
        muteButton?.setTitle(muted ? "Unmute" : "Mute", for: .normal)
        muteButton?.backgroundColor = muted ? .systemOrange : UIColor.white.withAlphaComponent(0.18)

        let videoOn = sipClient?.isLocalVideoEnabled ?? true
        videoToggleButton?.setTitle(videoOn ? "Video On" : "Video Off", for: .normal)
        videoToggleButton?.backgroundColor = videoOn ? UIColor.systemGreen.withAlphaComponent(0.7) : UIColor.white.withAlphaComponent(0.18)

        let camBack = (sipClient?.cameraPosition == .back)
        flipCameraButton?.setTitle(camBack ? "Cam: Back" : "Cam: Front", for: .normal)

        let speakerOn = sipClient?.isSpeakerEnabled ?? true
        speakerButton?.setTitle(speakerOn ? "Speaker" : "Earpiece", for: .normal)
        speakerButton?.backgroundColor = speakerOn ? UIColor.systemBlue.withAlphaComponent(0.7) : UIColor.white.withAlphaComponent(0.18)

        let held = sipCall?.isOnHold ?? false
        holdButton?.setTitle(held ? "Resume" : "Hold", for: .normal)
        holdButton?.backgroundColor = held ? .systemYellow.withAlphaComponent(0.75) : UIColor.white.withAlphaComponent(0.18)

        keypadButton?.backgroundColor = dtmfContainerView.isHidden ? UIColor.white.withAlphaComponent(0.18) : UIColor.systemPurple.withAlphaComponent(0.75)
        updateHeaderSubtitle()
    }

    // MARK: - Verification / Inspection Helpers

    var renderedRemoteFrames: UInt {
        remoteVideoView.renderedFrameCount
    }

    var renderedLocalFrames: UInt {
        localVideoView.renderedFrameCount
    }

    func exerciseAllControlsForVerification() -> Bool {
        toggleMuteTapped()
        let mutedOnce = (sipClient?.isMicrophoneMuted == true)
        toggleMuteTapped()
        let unmutedAgain = (sipClient?.isMicrophoneMuted == false)

        flipCameraTapped()
        let flippedToBack = (sipClient?.cameraPosition == .back)
        flipCameraTapped()
        let flippedToFront = (sipClient?.cameraPosition == .front)

        toggleVideoTapped()
        let videoDisabled = (sipClient?.isLocalVideoEnabled == false)
        toggleVideoTapped()
        let videoReenabled = (sipClient?.isLocalVideoEnabled == true)

        toggleSpeakerTapped()
        toggleSpeakerTapped()

        toggleKeypadTapped()
        if let call = sipCall {
            _ = call.sendDtmf("5")
        }

        return mutedOnce && unmutedAgain && flippedToBack && flippedToFront && videoDisabled && videoReenabled
    }
}
