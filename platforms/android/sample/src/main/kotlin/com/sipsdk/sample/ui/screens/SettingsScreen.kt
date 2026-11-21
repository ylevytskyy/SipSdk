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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.sipsdk.SipCameraPosition
import com.sipsdk.SipRegistrationState
import com.sipsdk.SipSurfaceView
import com.sipsdk.sample.models.SipAccount
import com.sipsdk.sample.ui.theme.IosBlue
import com.sipsdk.sample.ui.theme.IosGray
import com.sipsdk.sample.ui.theme.IosGreen
import com.sipsdk.sample.ui.theme.IosOrange
import com.sipsdk.sample.ui.theme.IosRed
import com.sipsdk.sample.viewmodels.SipAppViewModel

@Composable
fun SettingsScreen(
    viewModel: SipAppViewModel,
    onDismiss: (() -> Unit)? = null,
    modifier: Modifier = Modifier
) {
    val initialAccount = remember { viewModel.accountStore.account }
    var username by remember { mutableStateOf(initialAccount.username) }
    var domain by remember { mutableStateOf(initialAccount.domain) }
    var password by remember { mutableStateOf(initialAccount.password) }
    var displayName by remember { mutableStateOf(initialAccount.displayName ?: "") }
    var transport by remember { mutableStateOf(initialAccount.transport) }
    var portString by remember { mutableStateOf(initialAccount.port.toString()) }
    var outboundProxy by remember { mutableStateOf(initialAccount.outboundProxy ?: "") }
    var stunServer by remember { mutableStateOf(initialAccount.stunServer ?: "") }
    var useIce by remember { mutableStateOf(initialAccount.useIce) }
    var regIntervalString by remember { mutableStateOf(initialAccount.regInterval.toString()) }
    var savedToast by remember { mutableStateOf(false) }

    val regState by viewModel.registrationState.collectAsState()
    val regReason by viewModel.regReason.collectAsState()
    val isHandoverInProgress by viewModel.isHandoverInProgress.collectAsState()
    val handoverMessage by viewModel.handoverMessage.collectAsState()
    val isSpeakerEnabled by viewModel.isSpeakerEnabled.collectAsState()
    val cameraPosition by viewModel.cameraPosition.collectAsState()
    val isPreviewActive by viewModel.isPreviewActive.collectAsState()

    DisposableEffect(Unit) {
        onDispose {
            if (viewModel.isPreviewActive.value) {
                viewModel.setPreviewActive(false)
            }
        }
    }

    val statusColor = when (regState) {
        SipRegistrationState.REGISTERED -> IosGreen
        SipRegistrationState.REGISTERING -> IosOrange
        SipRegistrationState.FAILED -> IosRed
        SipRegistrationState.UNREGISTERED -> IosGray
    }

    val statusDescription = when (regState) {
        SipRegistrationState.REGISTERED -> "Online · Registered"
        SipRegistrationState.REGISTERING -> "Connecting..."
        SipRegistrationState.FAILED -> "Failed: $regReason"
        SipRegistrationState.UNREGISTERED -> "Offline"
    }

    val saveAndApply: () -> Unit = {
        val p = portString.toIntOrNull() ?: 5060
        val interval = regIntervalString.toIntOrNull() ?: 3600
        val updated = SipAccount(
            username = username.trim(),
            domain = domain.trim(),
            password = password,
            transport = transport,
            port = p,
            displayName = displayName.trim().takeIf { it.isNotEmpty() },
            outboundProxy = outboundProxy.trim().takeIf { it.isNotEmpty() },
            stunServer = stunServer.trim().takeIf { it.isNotEmpty() },
            useIce = useIce,
            regInterval = interval
        )
        viewModel.saveAccountAndRegister(updated)
        savedToast = true
        onDismiss?.invoke()
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp)
    ) {
        // Clean Tab Header (no modal Cancel/Save clutter)
        Text(
            text = "Settings",
            fontSize = 28.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = (-0.5).sp,
            color = MaterialTheme.colorScheme.onBackground
        )

        // Section 1: SIP Account Status
        SettingsSectionHeader(text = "CONNECTION")
        SettingsCard {
            Row(
                modifier = Modifier.padding(16.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(10.dp)
                        .clip(CircleShape)
                        .background(statusColor)
                )
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(2.dp)
                ) {
                    Text(
                        text = statusDescription,
                        fontSize = 15.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Text(
                        text = "${viewModel.accountStore.account.username} · ${viewModel.accountStore.account.domain}:${viewModel.accountStore.account.port}",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                if (regState == SipRegistrationState.REGISTERED) {
                    TextButton(onClick = { viewModel.unregister() }) {
                        Text(
                            text = "Disconnect",
                            color = IosRed,
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 13.sp
                        )
                    }
                } else {
                    TextButton(onClick = { viewModel.startAndRegister() }) {
                        Text(
                            text = "Connect",
                            color = IosBlue,
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 13.sp
                        )
                    }
                }
            }
        }

        // Section 2: Account Credentials
        SettingsSectionHeader(text = "SIP ACCOUNT")
        SettingsCard {
            Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                SettingsTextFieldRow(
                    label = "Username",
                    value = username,
                    onValueChange = { username = it; savedToast = false },
                    placeholder = "e.g. 1001"
                )
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Server",
                    value = domain,
                    onValueChange = { domain = it; savedToast = false },
                    placeholder = "e.g. 192.168.0.161"
                )
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Password",
                    value = password,
                    onValueChange = { password = it; savedToast = false },
                    placeholder = "Required",
                    isPassword = true
                )
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Display Name",
                    value = displayName,
                    onValueChange = { displayName = it; savedToast = false },
                    placeholder = "Optional"
                )
            }
        }

        // Section 3: Network & Transport
        SettingsSectionHeader(text = "NETWORK & TRANSPORT")
        SettingsCard {
            Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "Transport",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SegmentChip(
                            title = "UDP",
                            selected = transport.equals("udp", ignoreCase = true),
                            onClick = { transport = "udp"; savedToast = false }
                        )
                        SegmentChip(
                            title = "TCP",
                            selected = transport.equals("tcp", ignoreCase = true),
                            onClick = { transport = "tcp"; savedToast = false }
                        )
                        SegmentChip(
                            title = "TLS",
                            selected = transport.equals("tls", ignoreCase = true),
                            onClick = { transport = "tls"; savedToast = false }
                        )
                        SegmentChip(
                            title = "WS",
                            selected = transport.equals("ws", ignoreCase = true),
                            onClick = { transport = "ws"; savedToast = false }
                        )
                        SegmentChip(
                            title = "WSS",
                            selected = transport.equals("wss", ignoreCase = true),
                            onClick = { transport = "wss"; savedToast = false }
                        )
                    }
                }
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Port",
                    value = portString,
                    onValueChange = { portString = it; savedToast = false },
                    placeholder = "5060",
                    keyboardType = KeyboardType.Number
                )
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Outbound Proxy",
                    value = outboundProxy,
                    onValueChange = { outboundProxy = it; savedToast = false },
                    placeholder = "Optional"
                )
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "STUN Server",
                    value = stunServer,
                    onValueChange = { stunServer = it; savedToast = false },
                    placeholder = "Optional"
                )
                SettingsDivider()
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "ICE NAT Traversal",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Switch(
                        checked = useIce,
                        onCheckedChange = { useIce = it; savedToast = false },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = Color.White,
                            checkedTrackColor = IosGreen
                        )
                    )
                }
                SettingsDivider()
                SettingsTextFieldRow(
                    label = "Reg Interval",
                    value = regIntervalString,
                    onValueChange = { regIntervalString = it; savedToast = false },
                    placeholder = "3600",
                    keyboardType = KeyboardType.Number
                )
            }
        }

        // Primary Save & Reconnect CTA
        Button(
            onClick = saveAndApply,
            modifier = Modifier
                .fillMaxWidth()
                .height(50.dp),
            shape = RoundedCornerShape(14.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = IosBlue,
                contentColor = Color.White
            )
        ) {
            Icon(
                imageVector = Icons.Filled.Check,
                contentDescription = null,
                modifier = Modifier.size(18.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(
                text = if (savedToast) "Saved & Reconnecting" else "Save & Reconnect",
                fontSize = 15.sp,
                fontWeight = FontWeight.SemiBold
            )
        }

        // Section 4: Media Hardware (instant apply)
        SettingsSectionHeader(text = "AUDIO & VIDEO")
        SettingsCard {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "Camera",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        SegmentChip(
                            title = "Front",
                            selected = cameraPosition == SipCameraPosition.FRONT,
                            onClick = { viewModel.setCameraPosition(SipCameraPosition.FRONT) }
                        )
                        SegmentChip(
                            title = "Back",
                            selected = cameraPosition == SipCameraPosition.BACK,
                            onClick = { viewModel.setCameraPosition(SipCameraPosition.BACK) }
                        )
                    }
                }

                SettingsDivider()

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "Speakerphone Default",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Switch(
                        checked = isSpeakerEnabled,
                        onCheckedChange = { viewModel.toggleSpeaker() },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = Color.White,
                            checkedTrackColor = IosGreen
                        )
                    )
                }

                SettingsDivider()

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "Camera Preview",
                        fontSize = 14.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Switch(
                        checked = isPreviewActive,
                        onCheckedChange = { viewModel.setPreviewActive(it) },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = Color.White,
                            checkedTrackColor = IosGreen
                        )
                    )
                }

                if (isPreviewActive) {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(180.dp)
                            .clip(RoundedCornerShape(14.dp))
                            .border(
                                1.dp,
                                MaterialTheme.colorScheme.outline,
                                RoundedCornerShape(14.dp)
                            )
                            .background(Color.Black)
                    ) {
                        AndroidView(
                            factory = { ctx ->
                                SipSurfaceView(ctx).also { view ->
                                    viewModel.bindLocalVideo(view)
                                }
                            },
                            modifier = Modifier.fillMaxSize()
                        )
                    }
                }
            }
        }

        // Section 5: Network Handover & Reset
        SettingsSectionHeader(text = "DIAGNOSTICS")
        SettingsCard {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Button(
                    onClick = { viewModel.performNetworkHandover() },
                    enabled = !isHandoverInProgress,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(46.dp),
                    shape = RoundedCornerShape(12.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = IosBlue.copy(alpha = 0.12f),
                        contentColor = IosBlue
                    )
                ) {
                    if (isHandoverInProgress) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(16.dp),
                            color = IosBlue,
                            strokeWidth = 2.dp
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        Text("Re-binding Transports...", fontWeight = FontWeight.SemiBold)
                    } else {
                        Icon(
                            imageVector = Icons.Filled.Refresh,
                            contentDescription = null,
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(modifier = Modifier.width(6.dp))
                        Text("Reset Transports (Handover)", fontWeight = FontWeight.SemiBold)
                    }
                }

                if (handoverMessage != null) {
                    Text(
                        text = handoverMessage ?: "",
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium,
                        color = IosGreen
                    )
                }

                SettingsDivider()

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(10.dp))
                        .clickable {
                            viewModel.accountStore.resetToDefaults()
                            val def = viewModel.accountStore.account
                            username = def.username
                            domain = def.domain
                            password = def.password
                            displayName = def.displayName ?: ""
                            transport = def.transport
                            portString = def.port.toString()
                            outboundProxy = def.outboundProxy ?: ""
                            stunServer = def.stunServer ?: ""
                            useIce = def.useIce
                            regIntervalString = def.regInterval.toString()
                            savedToast = false
                        }
                        .padding(vertical = 8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "Reset Account to Defaults",
                        color = IosRed,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(12.dp))
    }
}

@Composable
private fun SettingsCard(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
        content = content
    )
}

@Composable
private fun SettingsSectionHeader(text: String) {
    Text(
        text = text,
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        letterSpacing = 0.8.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 6.dp)
    )
}

@Composable
private fun SettingsDivider() {
    HorizontalDivider(
        color = MaterialTheme.colorScheme.outline.copy(alpha = 0.6f),
        thickness = 0.75.dp
    )
}

@Composable
private fun SettingsTextFieldRow(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    isPassword: Boolean = false,
    keyboardType: KeyboardType = KeyboardType.Text
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            fontSize = 14.sp,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.width(114.dp)
        )
        OutlinedTextField(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier.weight(1f),
            placeholder = {
                Text(
                    text = placeholder,
                    fontSize = 14.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.55f)
                )
            },
            singleLine = true,
            visualTransformation = if (isPassword) PasswordVisualTransformation() else VisualTransformation.None,
            keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
            colors = OutlinedTextFieldDefaults.colors(
                focusedBorderColor = Color.Transparent,
                unfocusedBorderColor = Color.Transparent,
                focusedContainerColor = Color.Transparent,
                unfocusedContainerColor = Color.Transparent
            )
        )
    }
}

@Composable
private fun SegmentChip(
    title: String,
    selected: Boolean,
    onClick: () -> Unit
) {
    Surface(
        modifier = Modifier
            .clip(RoundedCornerShape(10.dp))
            .clickable(onClick = onClick),
        shape = RoundedCornerShape(10.dp),
        color = if (selected) IosBlue else MaterialTheme.colorScheme.surfaceVariant
    ) {
        Text(
            text = title,
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (selected) Color.White else MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp)
        )
    }
}
