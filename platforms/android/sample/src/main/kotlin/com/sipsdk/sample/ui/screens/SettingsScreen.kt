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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    viewModel: SipAppViewModel,
    onDismiss: () -> Unit,
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
        SipRegistrationState.REGISTERED -> "Registered"
        SipRegistrationState.REGISTERING -> "Registering..."
        SipRegistrationState.FAILED -> "Failed: $regReason"
        SipRegistrationState.UNREGISTERED -> "Unregistered"
    }

    val saveAndApply = {
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
        onDismiss()
    }

    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = {
                    Text(
                        text = "Settings",
                        fontWeight = FontWeight.SemiBold,
                        fontSize = 17.sp
                    )
                },
                navigationIcon = {
                    TextButton(onClick = onDismiss) {
                        Text(
                            text = "Cancel",
                            color = IosBlue,
                            fontSize = 16.sp
                        )
                    }
                },
                actions = {
                    TextButton(onClick = saveAndApply) {
                        Text(
                            text = "Save",
                            color = IosBlue,
                            fontWeight = FontWeight.Bold,
                            fontSize = 16.sp
                        )
                    }
                },
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background
                )
            )
        },
        containerColor = MaterialTheme.colorScheme.background,
        modifier = modifier
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp)
        ) {
            // Section 1: SIP Account Status
            SettingsSectionHeader(text = "SIP ACCOUNT STATUS")
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp)),
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Row(
                    modifier = Modifier.padding(16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(12.dp)
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
                            text = viewModel.accountStore.account.aor,
                            fontSize = 12.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }

                    if (regState == SipRegistrationState.REGISTERED) {
                        TextButton(onClick = { viewModel.unregister() }) {
                            Text(
                                text = "Unregister",
                                color = IosRed,
                                fontWeight = FontWeight.SemiBold,
                                fontSize = 14.sp
                            )
                        }
                    } else {
                        TextButton(onClick = { viewModel.startAndRegister() }) {
                            Text(
                                text = "Register",
                                color = IosBlue,
                                fontWeight = FontWeight.SemiBold,
                                fontSize = 14.sp
                            )
                        }
                    }
                }
            }

            // Section 2: Account Credentials
            SettingsSectionHeader(text = "CREDENTIALS")
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp)),
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                    SettingsTextFieldRow(
                        label = "Username",
                        value = username,
                        onValueChange = { username = it },
                        placeholder = "e.g. 1001"
                    )
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "Domain / Host",
                        value = domain,
                        onValueChange = { domain = it },
                        placeholder = "e.g. 10.0.2.2"
                    )
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "Password",
                        value = password,
                        onValueChange = { password = it },
                        placeholder = "Required",
                        isPassword = true
                    )
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "Display Name",
                        value = displayName,
                        onValueChange = { displayName = it },
                        placeholder = "Optional"
                    )
                }
            }

            // Section 3: Network & NAT
            SettingsSectionHeader(text = "NETWORK & NAT")
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp)),
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                    // Transport picker
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(
                            text = "Transport",
                            fontSize = 15.sp,
                            color = MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.width(110.dp)
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            SegmentChip(
                                title = "UDP",
                                selected = transport.equals("udp", ignoreCase = true),
                                onClick = { transport = "udp" }
                            )
                            SegmentChip(
                                title = "TCP",
                                selected = transport.equals("tcp", ignoreCase = true),
                                onClick = { transport = "tcp" }
                            )
                        }
                    }
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "Port",
                        value = portString,
                        onValueChange = { portString = it },
                        placeholder = "5060",
                        keyboardType = KeyboardType.Number
                    )
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "Outbound Proxy",
                        value = outboundProxy,
                        onValueChange = { outboundProxy = it },
                        placeholder = "Optional"
                    )
                    SettingsDivider()
                    SettingsTextFieldRow(
                        label = "STUN Server",
                        value = stunServer,
                        onValueChange = { stunServer = it },
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
                            text = "Enable ICE NAT Traversal",
                            fontSize = 15.sp,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                        Switch(
                            checked = useIce,
                            onCheckedChange = { useIce = it },
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
                        onValueChange = { regIntervalString = it },
                        placeholder = "3600",
                        keyboardType = KeyboardType.Number
                    )
                }
            }

            // Section 4: Enterprise Features
            SettingsSectionHeader(text = "ENTERPRISE FEATURES")
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp)),
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(
                    modifier = Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Text(
                        text = "Reset Transports simulates seamless switching between Wi-Fi and Cellular by re-binding sockets and refreshing SIP registration without dropping the engine.",
                        fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )

                    Button(
                        onClick = { viewModel.performNetworkHandover() },
                        enabled = !isHandoverInProgress,
                        modifier = Modifier.fillMaxWidth(),
                        shape = RoundedCornerShape(10.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = IosBlue,
                            contentColor = Color.White
                        )
                    ) {
                        if (isHandoverInProgress) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(18.dp),
                                color = Color.White,
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
                            Text("Perform Network Handover", fontWeight = FontWeight.SemiBold)
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
                }
            }

            // Section 5: Media Hardware
            SettingsSectionHeader(text = "MEDIA HARDWARE")
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp)),
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(
                            text = "Camera",
                            fontSize = 15.sp,
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
                            fontSize = 15.sp,
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
                            text = "Local Camera Preview Test",
                            fontSize = 15.sp,
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
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 8.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            Box(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .height(180.dp)
                                    .clip(RoundedCornerShape(12.dp))
                                    .border(
                                        1.dp,
                                        MaterialTheme.colorScheme.outline.copy(alpha = 0.5f),
                                        RoundedCornerShape(12.dp)
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
                            Text(
                                text = "Real-time Android Camera2 Local View",
                                fontSize = 11.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }

            // Section 6: Reset / Restore
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
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
                    },
                color = MaterialTheme.colorScheme.surface,
                shape = RoundedCornerShape(12.dp)
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = "Reset to Defaults",
                        color = IosRed,
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Medium
                    )
                }
            }

            Spacer(modifier = Modifier.height(24.dp))
        }
    }
}

@Composable
private fun SettingsSectionHeader(text: String) {
    Text(
        text = text,
        fontSize = 12.sp,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
        modifier = Modifier.padding(start = 12.dp)
    )
}

@Composable
private fun SettingsDivider() {
    HorizontalDivider(
        color = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f),
        thickness = 0.5.dp
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
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            fontSize = 15.sp,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.width(110.dp)
        )
        OutlinedTextField(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier.weight(1f),
            placeholder = { Text(placeholder, fontSize = 14.sp) },
            singleLine = true,
            visualTransformation = if (isPassword) PasswordVisualTransformation() else androidx.compose.ui.text.input.VisualTransformation.None,
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
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(if (selected) IosBlue else MaterialTheme.colorScheme.surfaceVariant)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = title,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            color = if (selected) Color.White else MaterialTheme.colorScheme.onSurface
        )
    }
}
