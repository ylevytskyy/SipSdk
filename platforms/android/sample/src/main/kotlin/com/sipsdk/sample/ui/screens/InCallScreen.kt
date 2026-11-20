package com.sipsdk.sample.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.BluetoothAudio
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.Dialpad
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.VideocamOff
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.sipsdk.SipCallState
import com.sipsdk.SipSurfaceView
import com.sipsdk.SipTelecomEndpoint
import com.sipsdk.sample.ui.theme.CallGradientBottom
import com.sipsdk.sample.ui.theme.CallGradientTop
import com.sipsdk.sample.ui.theme.IosBlue
import com.sipsdk.sample.ui.theme.IosGreen
import com.sipsdk.sample.ui.theme.IosRed
import com.sipsdk.sample.ui.theme.IosYellow
import com.sipsdk.sample.viewmodels.SipAppViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InCallScreen(
    viewModel: SipAppViewModel,
    modifier: Modifier = Modifier
) {
    val call by viewModel.activeCall.collectAsState()
    val openCalls by viewModel.openCalls.collectAsState()
    val callState by viewModel.activeCallState.collectAsState()
    val durationSeconds by viewModel.callDurationSeconds.collectAsState()
    val isOnHold by viewModel.callIsOnHold.collectAsState()
    val isMicMuted by viewModel.isMicrophoneMuted.collectAsState()
    val audioEndpoint by viewModel.audioEndpoint.collectAsState()
    val isLocalVideoEnabled by viewModel.isLocalVideoEnabled.collectAsState()
    val isShowingKeypad by viewModel.isShowingKeypadSheet.collectAsState()

    val hasVideo = call?.hasVideo == true
    val remoteTitle = call?.peerDisplayName ?: call?.remoteUri ?: "Unknown Caller"
    val remoteInitial = remoteTitle.trim().firstOrNull { it.isLetterOrDigit() }?.uppercaseChar()

    val statusSubtitle = when (callState) {
        SipCallState.INCOMING -> "Incoming Call..."
        SipCallState.RINGING -> "Ringing..."
        SipCallState.CONNECTING -> "Connecting..."
        SipCallState.ESTABLISHED -> {
            val m = durationSeconds / 60
            val s = durationSeconds % 60
            String.format("%02d:%02d", m, s)
        }
        SipCallState.HOLD -> "On Hold"
        SipCallState.CLOSED -> "Call Ended"
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(
                if (hasVideo) {
                    Brush.verticalGradient(listOf(Color.Black, Color.Black))
                } else {
                    Brush.verticalGradient(listOf(CallGradientTop, CallGradientBottom))
                }
            )
    ) {
        // Video / Audio Canvas
        if (hasVideo) {
            AndroidView(
                factory = { ctx ->
                    SipSurfaceView(ctx).also { view ->
                        viewModel.bindRemoteVideo(view)
                    }
                },
                onRelease = {
                    viewModel.bindRemoteVideo(null)
                },
                modifier = Modifier.fillMaxSize()
            )
        } else {
            Column(
                modifier = Modifier
                    .align(Alignment.Center)
                    .padding(bottom = 48.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(18.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(124.dp)
                        .clip(CircleShape)
                        .background(IosBlue.copy(alpha = 0.16f))
                        .border(1.5.dp, Color.White.copy(alpha = 0.16f), CircleShape),
                    contentAlignment = Alignment.Center
                ) {
                    Box(
                        modifier = Modifier
                            .size(96.dp)
                            .clip(CircleShape)
                            .background(
                                Brush.linearGradient(
                                    listOf(IosBlue, Color(0xFF1D4ED8))
                                )
                            ),
                        contentAlignment = Alignment.Center
                    ) {
                        if (remoteInitial != null) {
                            Text(
                                text = remoteInitial.toString(),
                                fontSize = 38.sp,
                                fontWeight = FontWeight.Bold,
                                color = Color.White
                            )
                        } else {
                            Icon(
                                imageVector = Icons.Filled.Person,
                                contentDescription = null,
                                tint = Color.White,
                                modifier = Modifier.size(48.dp)
                            )
                        }
                    }
                }

                Text(
                    text = remoteTitle,
                    fontSize = 26.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.White,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(horizontal = 24.dp)
                )
            }
        }

        // Top Header + Multi-line Switcher + Audio Route Selector
        Column(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .statusBarsPadding()
                .padding(top = 18.dp, start = 16.dp, end = 16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Surface(
                color = Color.White.copy(alpha = 0.10f),
                shape = RoundedCornerShape(24.dp),
                border = BorderStroke(1.dp, Color.White.copy(alpha = 0.14f))
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 18.dp, vertical = 9.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(8.dp)
                            .clip(CircleShape)
                            .background(if (isOnHold) IosYellow else IosGreen)
                    )
                    Text(
                        text = remoteTitle,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = Color.White,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    Text(
                        text = "•",
                        fontSize = 14.sp,
                        color = Color.White.copy(alpha = 0.5f)
                    )
                    Text(
                        text = statusSubtitle,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Medium,
                        color = Color.White.copy(alpha = 0.9f)
                    )
                }
            }

            if (openCalls.size > 1) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    openCalls.forEach { line ->
                        val isFocused = line.callId == call?.callId
                        Surface(
                            modifier = Modifier.clickable { viewModel.swapToCall(line) },
                            color = if (isFocused) IosBlue else Color.White.copy(alpha = 0.12f),
                            shape = RoundedCornerShape(18.dp),
                            border = BorderStroke(
                                1.dp,
                                if (isFocused) IosBlue else Color.White.copy(alpha = 0.18f)
                            )
                        ) {
                            Text(
                                text = "${line.peerDisplayName ?: line.remoteUri} • ${if (line.isOnHold) "Hold" else "Active"}",
                                fontSize = 12.sp,
                                fontWeight = FontWeight.SemiBold,
                                color = Color.White,
                                modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp)
                            )
                        }
                    }
                    val other = openCalls.firstOrNull { it.callId != call?.callId }
                    if (other != null) {
                        Surface(
                            modifier = Modifier.clickable { viewModel.swapToCall(other) },
                            color = Color.White.copy(alpha = 0.22f),
                            shape = RoundedCornerShape(18.dp)
                        ) {
                            Row(
                                modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(4.dp)
                            ) {
                                Icon(
                                    imageVector = Icons.Filled.SwapHoriz,
                                    contentDescription = null,
                                    tint = Color.White,
                                    modifier = Modifier.size(14.dp)
                                )
                                Text(
                                    text = "Swap",
                                    fontSize = 12.sp,
                                    fontWeight = FontWeight.Bold,
                                    color = Color.White
                                )
                            }
                        }
                    }
                }
            }

            // Frosted Audio Route Selector
            Surface(
                color = Color.White.copy(alpha = 0.08f),
                shape = RoundedCornerShape(22.dp),
                border = BorderStroke(1.dp, Color.White.copy(alpha = 0.12f))
            ) {
                Row(
                    modifier = Modifier.padding(4.dp),
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    listOf(
                        Triple(SipTelecomEndpoint.EARPIECE, "Earpiece", Icons.Filled.Hearing),
                        Triple(SipTelecomEndpoint.SPEAKER, "Speaker", Icons.AutoMirrored.Filled.VolumeUp),
                        Triple(SipTelecomEndpoint.BLUETOOTH, "Bluetooth", Icons.Filled.BluetoothAudio)
                    ).forEach { (ep, label, icon) ->
                        val selected = audioEndpoint == ep
                        Surface(
                            modifier = Modifier.clickable { viewModel.selectAudioEndpoint(ep) },
                            color = if (selected) IosBlue else Color.Transparent,
                            shape = RoundedCornerShape(18.dp)
                        ) {
                            Row(
                                modifier = Modifier.padding(horizontal = 12.dp, vertical = 7.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(5.dp)
                            ) {
                                Icon(
                                    imageVector = icon,
                                    contentDescription = null,
                                    tint = if (selected) Color.White else Color.White.copy(alpha = 0.72f),
                                    modifier = Modifier.size(14.dp)
                                )
                                Text(
                                    text = label,
                                    fontSize = 12.sp,
                                    fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                                    color = if (selected) Color.White else Color.White.copy(alpha = 0.78f)
                                )
                            }
                        }
                    }
                }
            }
        }

        // On-Hold Overlay
        if (isOnHold) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.55f)),
                contentAlignment = Alignment.Center
            ) {
                Surface(
                    color = Color(0xFF1E293B).copy(alpha = 0.92f),
                    shape = RoundedCornerShape(22.dp),
                    border = BorderStroke(1.dp, IosYellow.copy(alpha = 0.35f))
                ) {
                    Column(
                        modifier = Modifier.padding(horizontal = 28.dp, vertical = 20.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.Pause,
                            contentDescription = "Hold",
                            tint = IosYellow,
                            modifier = Modifier.size(40.dp)
                        )
                        Text(
                            text = "Call on Hold",
                            fontSize = 18.sp,
                            fontWeight = FontWeight.Bold,
                            color = Color.White
                        )
                    }
                }
            }
        }

        // Floating Local Preview PIP (bottom right)
        if (hasVideo && isLocalVideoEnabled) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .navigationBarsPadding()
                    .padding(end = 18.dp, bottom = 184.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(width = 104.dp, height = 144.dp)
                        .shadow(10.dp, RoundedCornerShape(16.dp))
                        .clip(RoundedCornerShape(16.dp))
                        .border(1.5.dp, Color.White.copy(alpha = 0.45f), RoundedCornerShape(16.dp))
                        .background(Color.Black)
                ) {
                    AndroidView(
                        factory = { ctx ->
                            SipSurfaceView(ctx).apply {
                                setZOrderMediaOverlay(true)
                                clipToOutline = true
                            }.also { view ->
                                viewModel.bindLocalVideo(view)
                            }
                        },
                        onRelease = {
                            viewModel.bindLocalVideo(null)
                        },
                        modifier = Modifier.fillMaxSize()
                    )

                    IconButton(
                        onClick = { viewModel.switchCamera() },
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(5.dp)
                            .size(28.dp)
                            .clip(CircleShape)
                            .background(Color.Black.copy(alpha = 0.55f))
                    ) {
                        Icon(
                            imageVector = Icons.Filled.Cameraswitch,
                            contentDescription = "Flip Camera",
                            tint = Color.White,
                            modifier = Modifier.size(16.dp)
                        )
                    }
                }
            }
        }

        // Bottom Controls Dock
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(horizontal = 20.dp, vertical = 22.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(18.dp)
        ) {
            Surface(
                color = Color.White.copy(alpha = 0.10f),
                shape = RoundedCornerShape(28.dp),
                border = BorderStroke(1.dp, Color.White.copy(alpha = 0.14f))
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 18.dp, vertical = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(18.dp)
                ) {
                    CallControlButton(
                        icon = if (isMicMuted) Icons.Filled.MicOff else Icons.Filled.Mic,
                        label = if (isMicMuted) "Unmute" else "Mute",
                        isActive = isMicMuted,
                        onClick = { viewModel.toggleMute() }
                    )

                    CallControlButton(
                        icon = if (isLocalVideoEnabled) Icons.Filled.Videocam else Icons.Filled.VideocamOff,
                        label = "Video",
                        isActive = !isLocalVideoEnabled,
                        onClick = { viewModel.toggleVideo() }
                    )

                    CallControlButton(
                        icon = Icons.Filled.Dialpad,
                        label = "Keypad",
                        isActive = false,
                        onClick = { viewModel.setShowKeypadSheet(true) }
                    )

                    CallControlButton(
                        icon = if (isOnHold) Icons.Filled.PlayArrow else Icons.Filled.Pause,
                        label = if (isOnHold) "Resume" else "Hold",
                        isActive = isOnHold,
                        onClick = { viewModel.toggleHold() }
                    )
                }
            }

            // Prominent End Call Button
            IconButton(
                onClick = { viewModel.hangupActiveCall() },
                modifier = Modifier
                    .size(66.dp)
                    .shadow(10.dp, CircleShape)
                    .clip(CircleShape)
                    .background(IosRed)
            ) {
                Icon(
                    imageVector = Icons.Filled.CallEnd,
                    contentDescription = "Hangup",
                    tint = Color.White,
                    modifier = Modifier.size(30.dp)
                )
            }
        }

        // In-Call DTMF Keypad Bottom Sheet
        if (isShowingKeypad) {
            ModalBottomSheet(
                onDismissRequest = { viewModel.setShowKeypadSheet(false) },
                sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
            ) {
                DtmfKeypadSheet(
                    onDigit = { digit -> viewModel.sendDtmf(digit) },
                    onDismiss = { viewModel.setShowKeypadSheet(false) }
                )
            }
        }
    }
}

@Composable
private fun CallControlButton(
    icon: ImageVector,
    label: String,
    isActive: Boolean,
    onClick: () -> Unit
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        IconButton(
            onClick = onClick,
            modifier = Modifier
                .size(50.dp)
                .clip(CircleShape)
                .background(if (isActive) Color.White else Color.White.copy(alpha = 0.14f))
        ) {
            Icon(
                imageVector = icon,
                contentDescription = label,
                tint = if (isActive) Color(0xFF0F172A) else Color.White,
                modifier = Modifier.size(22.dp)
            )
        }
        Text(
            text = label,
            fontSize = 11.sp,
            fontWeight = FontWeight.Medium,
            color = Color.White.copy(alpha = 0.85f)
        )
    }
}

@Composable
private fun DtmfKeypadSheet(
    onDigit: (String) -> Unit,
    onDismiss: () -> Unit
) {
    val keys = listOf(
        listOf("1", "2", "3"),
        listOf("4", "5", "6"),
        listOf("7", "8", "9"),
        listOf("*", "0", "#")
    )

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 24.dp)
            .padding(bottom = 36.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(
                text = "Dialpad (DTMF)",
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold
            )
            TextButton(onClick = onDismiss) {
                Text(
                    text = "Done",
                    fontSize = 16.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = IosBlue
                )
            }
        }

        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            keys.forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceEvenly
                ) {
                    row.forEach { digit ->
                        Box(
                            modifier = Modifier
                                .size(68.dp)
                                .clip(CircleShape)
                                .background(Color.Gray.copy(alpha = 0.16f))
                                .clickable { onDigit(digit) },
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = digit,
                                fontSize = 26.sp,
                                fontWeight = FontWeight.SemiBold
                            )
                        }
                    }
                }
            }
        }
    }
}
