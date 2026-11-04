package com.sipsdk.sample.ui.screens

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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeDown
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.Dialpad
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.VideocamOff
import androidx.compose.material.icons.filled.VolumeDown
import androidx.compose.material.icons.filled.VolumeUp
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.sipsdk.SipCallState
import com.sipsdk.SipSurfaceView
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
    val callState by viewModel.activeCallState.collectAsState()
    val durationSeconds by viewModel.callDurationSeconds.collectAsState()
    val isOnHold by viewModel.callIsOnHold.collectAsState()
    val isMicMuted by viewModel.isMicrophoneMuted.collectAsState()
    val isSpeakerEnabled by viewModel.isSpeakerEnabled.collectAsState()
    val isLocalVideoEnabled by viewModel.isLocalVideoEnabled.collectAsState()
    val isShowingKeypad by viewModel.isShowingKeypadSheet.collectAsState()

    val hasVideo = call?.hasVideo == true
    val remoteTitle = call?.peerDisplayName ?: call?.remoteUri ?: "Unknown Caller"

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
            .background(if (hasVideo) Color.Transparent else Color.Black)
    ) {
        // Video / Audio Background
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
                modifier = Modifier.align(Alignment.Center),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Icon(
                    imageVector = Icons.Filled.AccountCircle,
                    contentDescription = null,
                    tint = Color.Gray,
                    modifier = Modifier.size(96.dp)
                )
                Text(
                    text = remoteTitle,
                    fontSize = 24.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.White
                )
            }
        }

        // Top Header
        Box(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = 44.dp)
        ) {
            Column(
                modifier = Modifier
                    .clip(RoundedCornerShape(20.dp))
                    .background(Color.Black.copy(alpha = 0.4f))
                    .padding(horizontal = 24.dp, vertical = 10.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                Text(
                    text = remoteTitle,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    color = Color.White
                )
                Text(
                    text = statusSubtitle,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Medium,
                    color = Color.White.copy(alpha = 0.85f)
                )
            }
        }

        // On-Hold Overlay
        if (isOnHold) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.6f)),
                contentAlignment = Alignment.Center
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        imageVector = Icons.Filled.Pause,
                        contentDescription = "Hold",
                        tint = IosYellow,
                        modifier = Modifier.size(56.dp)
                    )
                    Text(
                        text = "Call on Hold",
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        color = Color.White
                    )
                }
            }
        }

        // Floating Local Preview PIP (bottom right)
        if (hasVideo && isLocalVideoEnabled) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(end = 20.dp, bottom = 120.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(width = 100.dp, height = 140.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .border(1.5.dp, Color.White.copy(alpha = 0.6f), RoundedCornerShape(12.dp))
                        .shadow(8.dp)
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

                    // Switch camera mini-button
                    IconButton(
                        onClick = { viewModel.switchCamera() },
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(4.dp)
                            .size(28.dp)
                            .clip(CircleShape)
                            .background(Color.Black.copy(alpha = 0.5f))
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

        // Floating Bottom Controls Pill
        Surface(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = 36.dp, start = 16.dp, end = 16.dp)
                .clip(RoundedCornerShape(32.dp)),
            color = Color.Black.copy(alpha = 0.55f),
            shape = RoundedCornerShape(32.dp)
        ) {
            Row(
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(14.dp)
            ) {
                // Mute
                CallControlButton(
                    icon = if (isMicMuted) Icons.Filled.MicOff else Icons.Filled.Mic,
                    isActive = isMicMuted,
                    onClick = { viewModel.toggleMute() }
                )

                // Video toggle
                CallControlButton(
                    icon = if (isLocalVideoEnabled) Icons.Filled.Videocam else Icons.Filled.VideocamOff,
                    isActive = !isLocalVideoEnabled,
                    onClick = { viewModel.toggleVideo() }
                )

                // Keypad
                CallControlButton(
                    icon = Icons.Filled.Dialpad,
                    isActive = false,
                    onClick = { viewModel.setShowKeypadSheet(true) }
                )

                // Speaker
                CallControlButton(
                    icon = if (isSpeakerEnabled) Icons.AutoMirrored.Filled.VolumeUp else Icons.AutoMirrored.Filled.VolumeDown,
                    isActive = isSpeakerEnabled,
                    onClick = { viewModel.toggleSpeaker() }
                )

                // Hold
                CallControlButton(
                    icon = if (isOnHold) Icons.Filled.PlayArrow else Icons.Filled.Pause,
                    isActive = isOnHold,
                    onClick = { viewModel.toggleHold() }
                )

                // Hangup button
                IconButton(
                    onClick = { viewModel.hangupActiveCall() },
                    modifier = Modifier
                        .size(54.dp)
                        .clip(CircleShape)
                        .background(IosRed)
                ) {
                    Icon(
                        imageVector = Icons.Filled.CallEnd,
                        contentDescription = "Hangup",
                        tint = Color.White,
                        modifier = Modifier.size(26.dp)
                    )
                }
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
    isActive: Boolean,
    onClick: () -> Unit
) {
    IconButton(
        onClick = onClick,
        modifier = Modifier
            .size(46.dp)
            .clip(CircleShape)
            .background(if (isActive) Color.White else Color.White.copy(alpha = 0.2f))
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = if (isActive) Color.Black else Color.White,
            modifier = Modifier.size(20.dp)
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
                    fontWeight = FontWeight.SemiBold
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
                                .background(Color.Gray.copy(alpha = 0.2f))
                                .clickable { onDigit(digit) },
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = digit,
                                fontSize = 28.sp,
                                fontWeight = FontWeight.SemiBold
                            )
                        }
                    }
                }
            }
        }
    }
}
