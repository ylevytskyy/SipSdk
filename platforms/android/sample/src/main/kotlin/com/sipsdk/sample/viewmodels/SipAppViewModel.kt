package com.sipsdk.sample.viewmodels

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.sipsdk.SipCall
import com.sipsdk.SipCallState
import com.sipsdk.SipCameraPosition
import com.sipsdk.SipClient
import com.sipsdk.SipClientListener
import com.sipsdk.SipRegistrationState
import com.sipsdk.SipSurfaceView
import com.sipsdk.sample.models.AccountStore
import com.sipsdk.sample.models.CallDirection
import com.sipsdk.sample.models.CallHistoryItem
import com.sipsdk.sample.models.CallHistoryStore
import com.sipsdk.sample.models.SipAccount
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

class SipAppViewModel(application: Application) : AndroidViewModel(application), SipClientListener {

    val accountStore = AccountStore(application)
    private val historyStore = CallHistoryStore(application)

    private var sipClient: SipClient? = null

    // UI States
    private val _selectedTab = MutableStateFlow(0)
    val selectedTab: StateFlow<Int> = _selectedTab.asStateFlow()

    private val _dialDestination = MutableStateFlow("9001")
    val dialDestination: StateFlow<String> = _dialDestination.asStateFlow()

    private val _registrationState = MutableStateFlow(SipRegistrationState.UNREGISTERED)
    val registrationState: StateFlow<SipRegistrationState> = _registrationState.asStateFlow()

    private val _regReason = MutableStateFlow("")
    val regReason: StateFlow<String> = _regReason.asStateFlow()

    private val _regCode = MutableStateFlow(0)
    val regCode: StateFlow<Int> = _regCode.asStateFlow()

    private val _activeCall = MutableStateFlow<SipCall?>(null)
    val activeCall: StateFlow<SipCall?> = _activeCall.asStateFlow()

    private val _activeCallState = MutableStateFlow(SipCallState.CLOSED)
    val activeCallState: StateFlow<SipCallState> = _activeCallState.asStateFlow()

    private val _incomingCall = MutableStateFlow<SipCall?>(null)
    val incomingCall: StateFlow<SipCall?> = _incomingCall.asStateFlow()

    private val _isShowingInCallView = MutableStateFlow(false)
    val isShowingInCallView: StateFlow<Boolean> = _isShowingInCallView.asStateFlow()

    private val _isShowingAccountSheet = MutableStateFlow(false)
    val isShowingAccountSheet: StateFlow<Boolean> = _isShowingAccountSheet.asStateFlow()

    private val _isShowingKeypadSheet = MutableStateFlow(false)
    val isShowingKeypadSheet: StateFlow<Boolean> = _isShowingKeypadSheet.asStateFlow()

    private val _callDurationSeconds = MutableStateFlow(0L)
    val callDurationSeconds: StateFlow<Long> = _callDurationSeconds.asStateFlow()

    private val _callIsOnHold = MutableStateFlow(false)
    val callIsOnHold: StateFlow<Boolean> = _callIsOnHold.asStateFlow()

    private val _isMicrophoneMuted = MutableStateFlow(false)
    val isMicrophoneMuted: StateFlow<Boolean> = _isMicrophoneMuted.asStateFlow()

    private val _isSpeakerEnabled = MutableStateFlow(false)
    val isSpeakerEnabled: StateFlow<Boolean> = _isSpeakerEnabled.asStateFlow()

    private val _isLocalVideoEnabled = MutableStateFlow(true)
    val isLocalVideoEnabled: StateFlow<Boolean> = _isLocalVideoEnabled.asStateFlow()

    private val _cameraPosition = MutableStateFlow(SipCameraPosition.FRONT)
    val cameraPosition: StateFlow<SipCameraPosition> = _cameraPosition.asStateFlow()

    private val _history = MutableStateFlow<List<CallHistoryItem>>(emptyList())
    val history: StateFlow<List<CallHistoryItem>> = _history.asStateFlow()

    private val _handoverMessage = MutableStateFlow<String?>(null)
    val handoverMessage: StateFlow<String?> = _handoverMessage.asStateFlow()

    private val _isHandoverInProgress = MutableStateFlow(false)
    val isHandoverInProgress: StateFlow<Boolean> = _isHandoverInProgress.asStateFlow()

    private val _isPreviewActive = MutableStateFlow(false)
    val isPreviewActive: StateFlow<Boolean> = _isPreviewActive.asStateFlow()

    private var durationTimerJob: Job? = null

    init {
        _history.value = historyStore.loadHistory()
        initClient()
    }

    fun setSelectedTab(tab: Int) {
        _selectedTab.value = tab
    }

    fun setDialDestination(dest: String) {
        _dialDestination.value = dest
    }

    fun setShowAccountSheet(show: Boolean) {
        _isShowingAccountSheet.value = show
    }

    fun setShowKeypadSheet(show: Boolean) {
        _isShowingKeypadSheet.value = show
    }

    fun setShowInCallView(show: Boolean) {
        _isShowingInCallView.value = show
    }

    fun setPreviewActive(active: Boolean) {
        _isPreviewActive.value = active
        if (active) {
            sipClient?.startLocalPreview()
        } else {
            sipClient?.stopLocalPreview()
        }
    }

    fun initClient() {
        val account = accountStore.account
        val client = SipClient(account.username, account.domain, getApplication()).apply {
            password = account.password
            displayName = account.displayName
            transport = account.transport
            port = account.port
            listener = this@SipAppViewModel
        }
        client.start()
        sipClient = client
    }

    fun bindRemoteVideo(view: SipSurfaceView?) {
        sipClient?.remoteVideoView = view
    }

    fun bindLocalVideo(view: SipSurfaceView?) {
        sipClient?.localVideoView = view
    }

    fun startAndRegister() {
        val client = sipClient ?: return
        _registrationState.value = SipRegistrationState.REGISTERING
        client.register()
    }

    fun unregister() {
        sipClient?.unregister()
        _registrationState.value = SipRegistrationState.UNREGISTERED
    }

    fun saveAccountAndRegister(account: SipAccount) {
        accountStore.account = account
        sipClient?.stop()
        initClient()
        startAndRegister()
    }

    fun makeCall(destination: String, withVideo: Boolean = true) {
        android.util.Log.i("SipAppViewModel", "makeCall called with destination: $destination, withVideo: $withVideo")
        val trimmed = destination.trim()
        if (trimmed.isEmpty()) return

        val client = sipClient ?: return
        val target = if (trimmed.startsWith("sip:") || trimmed.contains("@")) {
            trimmed
        } else {
            "sip:$trimmed@${accountStore.account.domain}:${accountStore.account.port}"
        }

        val call = client.makeCall(target, withVideo = withVideo)
        if (call != null) {
            _activeCall.value = call
            _activeCallState.value = SipCallState.CONNECTING
            _callDurationSeconds.value = 0L
            _callIsOnHold.value = false
            _isShowingInCallView.value = true
            if (withVideo) {
                _isSpeakerEnabled.value = true
                client.isSpeakerEnabled = true
            }

            // Record to history
            val item = CallHistoryItem(
                remoteUri = call.remoteUri,
                displayName = call.peerDisplayName,
                timestamp = System.currentTimeMillis(),
                durationSeconds = 0L,
                direction = CallDirection.OUTGOING,
                hasVideo = withVideo
            )
            addHistoryItem(item)
            startDurationTimer()
        }
    }

    fun answerIncomingCall(withVideo: Boolean = false) {
        val call = _incomingCall.value ?: return
        _incomingCall.value = null
        call.answer(withVideo = withVideo)
        _activeCall.value = call
        _activeCallState.value = SipCallState.CONNECTING
        _callDurationSeconds.value = 0L
        _isShowingInCallView.value = true
        startDurationTimer()
    }

    fun declineIncomingCall() {
        val call = _incomingCall.value ?: return
        _incomingCall.value = null
        call.hangup(statusCode = 603, reason = "Decline")
        _activeCall.value = null
        _isShowingInCallView.value = false
        stopDurationTimer()
    }

    fun hangupActiveCall() {
        _activeCall.value?.hangup()
        _activeCall.value = null
        _activeCallState.value = SipCallState.CLOSED
        _isShowingInCallView.value = false
        stopDurationTimer()
    }

    fun toggleMute() {
        val current = _isMicrophoneMuted.value
        val next = !current
        _isMicrophoneMuted.value = next
        sipClient?.isMicrophoneMuted = next
    }

    fun toggleSpeaker() {
        val current = _isSpeakerEnabled.value
        val next = !current
        _isSpeakerEnabled.value = next
        sipClient?.isSpeakerEnabled = next
    }

    fun toggleVideo() {
        val current = _isLocalVideoEnabled.value
        val next = !current
        _isLocalVideoEnabled.value = next
        sipClient?.isLocalVideoEnabled = next
    }

    fun switchCamera() {
        sipClient?.switchCamera()
        _cameraPosition.value = sipClient?.cameraPosition ?: SipCameraPosition.FRONT
    }

    fun setCameraPosition(position: SipCameraPosition) {
        _cameraPosition.value = position
        sipClient?.cameraPosition = position
    }

    fun toggleHold() {
        val call = _activeCall.value ?: return
        val next = !_callIsOnHold.value
        val rc = call.setHold(next)
        if (rc == 0) {
            _callIsOnHold.value = next
        }
    }

    fun sendDtmf(digit: String) {
        _activeCall.value?.sendDtmf(digit)
    }

    fun performNetworkHandover() {
        viewModelScope.launch {
            _isHandoverInProgress.value = true
            _handoverMessage.value = null
            sipClient?.resetTransports(reRegister = true)
            _handoverMessage.value = "Transports reset & re-registration initiated"
            _isHandoverInProgress.value = false
        }
    }

    fun clearHistory() {
        _history.value = emptyList()
        historyStore.saveHistory(emptyList())
    }

    fun removeHistoryItem(item: CallHistoryItem) {
        val list = _history.value.toMutableList()
        list.removeAll { it.id == item.id }
        _history.value = list
        historyStore.saveHistory(list)
    }

    private fun addHistoryItem(item: CallHistoryItem) {
        val list = mutableListOf(item)
        list.addAll(_history.value)
        _history.value = list
        historyStore.saveHistory(list)
    }

    private fun startDurationTimer() {
        durationTimerJob?.cancel()
        durationTimerJob = viewModelScope.launch {
            while (isActive) {
                delay(1000)
                val dur = _activeCall.value?.durationSeconds ?: (_callDurationSeconds.value + 1)
                _callDurationSeconds.value = dur
            }
        }
    }

    private fun stopDurationTimer() {
        durationTimerJob?.cancel()
        durationTimerJob = null
    }

    // SipClientListener callbacks
    override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
        viewModelScope.launch {
            _registrationState.value = state
            _regCode.value = code
            _regReason.value = reason
        }
    }

    override fun onIncomingCall(call: SipCall) {
        viewModelScope.launch {
            _incomingCall.value = call
            val item = CallHistoryItem(
                remoteUri = call.remoteUri,
                displayName = call.peerDisplayName,
                timestamp = System.currentTimeMillis(),
                durationSeconds = 0L,
                direction = CallDirection.INCOMING,
                hasVideo = call.hasVideo
            )
            addHistoryItem(item)
        }
    }

    override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
        viewModelScope.launch {
            _activeCallState.value = state
            if (state == SipCallState.ESTABLISHED) {
                _activeCall.value = call
                _isShowingInCallView.value = true
                startDurationTimer()
            } else if (state == SipCallState.CLOSED) {
                stopDurationTimer()
                if (_incomingCall.value?.callId == call.callId) {
                    _incomingCall.value = null
                }
                _activeCall.value = null
                _isShowingInCallView.value = false
                if (_isSpeakerEnabled.value) {
                    _isSpeakerEnabled.value = false
                    sipClient?.isSpeakerEnabled = false
                }
                // Update duration in history
                val finalDuration = call.durationSeconds
                val list = _history.value.toMutableList()
                val idx = list.indexOfFirst { it.remoteUri == call.remoteUri }
                if (idx != -1) {
                    val prev = list[idx]
                    list[idx] = prev.copy(durationSeconds = maxOf(prev.durationSeconds, finalDuration))
                    _history.value = list
                    historyStore.saveHistory(list)
                }
            }
        }
    }

    override fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
        viewModelScope.launch {
            _callIsOnHold.value = onHold
            _isMicrophoneMuted.value = isMuted
            if (width > 0 && height > 0) {
                _activeCall.value = call
            }
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopDurationTimer()
        sipClient?.destroy()
        sipClient = null
    }
}
