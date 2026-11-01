package com.sipsdk.sample

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.sipsdk.SipCall
import com.sipsdk.SipCallState
import com.sipsdk.SipClient
import com.sipsdk.SipClientListener
import com.sipsdk.SipRegistrationState
import com.sipsdk.SipSurfaceView

class MainActivity : AppCompatActivity(), SipClientListener {

    private var sipClient: SipClient? = null
    private var activeCall: SipCall? = null

    private lateinit var statusText: TextView
    private lateinit var callStatusText: TextView
    private lateinit var destinationInput: EditText
    private lateinit var registerBtn: Button
    private lateinit var callBtn: Button
    private lateinit var videoCallBtn: Button
    private lateinit var flipCameraBtn: Button
    private lateinit var hangupBtn: Button

    private lateinit var remoteVideoView: SipSurfaceView
    private lateinit var localVideoView: SipSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 48, 32, 32)
        }

        statusText = TextView(this).apply {
            text = "Status: Unregistered"
            textSize = 16f
        }
        layout.addView(statusText)

        registerBtn = Button(this).apply {
            text = "Register (1001@10.0.2.2)"
            setOnClickListener {
                registerAccount()
            }
        }
        layout.addView(registerBtn)

        destinationInput = EditText(this).apply {
            setText("sip:9001@10.0.2.2:5060")
            hint = "Destination SIP URI"
        }
        layout.addView(destinationInput)

        val btnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        callBtn = Button(this).apply {
            text = "Audio Call"
            setOnClickListener {
                val target = destinationInput.text.toString()
                activeCall = sipClient?.makeCall(target, withVideo = false)
            }
        }
        btnRow.addView(callBtn)

        videoCallBtn = Button(this).apply {
            text = "Video Call (9001)"
            setOnClickListener {
                val target = destinationInput.text.toString()
                activeCall = sipClient?.makeCall(target, withVideo = true)
            }
        }
        btnRow.addView(videoCallBtn)
        layout.addView(btnRow)

        val controlRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        flipCameraBtn = Button(this).apply {
            text = "Flip Camera"
            setOnClickListener {
                sipClient?.switchCamera()
            }
        }
        controlRow.addView(flipCameraBtn)

        hangupBtn = Button(this).apply {
            text = "Hangup"
            isEnabled = false
            setOnClickListener {
                activeCall?.hangup()
            }
        }
        controlRow.addView(hangupBtn)
        layout.addView(controlRow)

        callStatusText = TextView(this).apply {
            text = "Call: Idle"
            textSize = 14f
        }
        layout.addView(callStatusText)

        // Video views
        remoteVideoView = SipSurfaceView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                400
            )
        }
        layout.addView(remoteVideoView)

        localVideoView = SipSurfaceView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                240
            )
        }
        layout.addView(localVideoView)

        setContentView(layout)

        initSipClient()
    }

    private fun initSipClient() {
        val client = SipClient("1001", "10.0.2.2", this).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
            listener = this@MainActivity
        }
        client.start()
        client.remoteVideoView = remoteVideoView
        client.localVideoView = localVideoView
        client.startLocalPreview()
        sipClient = client
    }

    private fun registerAccount() {
        sipClient?.register()
    }

    override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
        statusText.text = "Status: $state ($code $reason)"
        registerBtn.isEnabled = (state != SipRegistrationState.REGISTERED)
    }

    override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
        callStatusText.text = "Call: $state ($code $reason)"
        hangupBtn.isEnabled = (state != SipCallState.CLOSED)
        callBtn.isEnabled = (state == SipCallState.CLOSED)
        videoCallBtn.isEnabled = (state == SipCallState.CLOSED)
        if (state == SipCallState.CLOSED) {
            activeCall = null
        }
    }

    override fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
        if (width > 0 && height > 0) {
            callStatusText.text = "Call Media: ${width}x${height} (Hold=$onHold, Muted=$isMuted)"
        }
    }

    override fun onIncomingCall(call: SipCall) {
        activeCall = call
        call.answer(withVideo = true)
    }

    override fun onDestroy() {
        super.onDestroy()
        sipClient?.destroy()
        sipClient = null
    }
}
