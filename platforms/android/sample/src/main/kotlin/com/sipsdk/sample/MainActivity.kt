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

class MainActivity : AppCompatActivity(), SipClientListener {

    private var sipClient: SipClient? = null
    private var activeCall: SipCall? = null

    private lateinit var statusText: TextView
    private lateinit var callStatusText: TextView
    private lateinit var destinationInput: EditText
    private lateinit var registerBtn: Button
    private lateinit var callBtn: Button
    private lateinit var hangupBtn: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 64, 32, 32)
        }

        statusText = TextView(this).apply {
            text = "Status: Unregistered"
            textSize = 18f
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

        callBtn = Button(this).apply {
            text = "Call Echo (9001)"
            setOnClickListener {
                val target = destinationInput.text.toString()
                activeCall = sipClient?.makeCall(target)
            }
        }
        layout.addView(callBtn)

        hangupBtn = Button(this).apply {
            text = "Hangup"
            isEnabled = false
            setOnClickListener {
                activeCall?.hangup()
            }
        }
        layout.addView(hangupBtn)

        callStatusText = TextView(this).apply {
            text = "Call: Idle"
            textSize = 16f
        }
        layout.addView(callStatusText)

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
        if (state == SipCallState.CLOSED) {
            activeCall = null
        }
    }

    override fun onIncomingCall(call: SipCall) {
        activeCall = call
        call.answer()
    }

    override fun onDestroy() {
        super.onDestroy()
        sipClient?.destroy()
        sipClient = null
    }
}
