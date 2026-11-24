package com.sipsdk.sample.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Backspace
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Dialpad
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.sipsdk.SipRegistrationState
import com.sipsdk.sample.ui.theme.IosBlue
import com.sipsdk.sample.ui.theme.IosGray
import com.sipsdk.sample.ui.theme.IosGreen
import com.sipsdk.sample.ui.theme.IosOrange
import com.sipsdk.sample.ui.theme.IosRed
import com.sipsdk.sample.viewmodels.SipAppViewModel

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun HomeScreen(
    viewModel: SipAppViewModel,
    modifier: Modifier = Modifier
) {
    val dialDestination by viewModel.dialDestination.collectAsState()
    val regState by viewModel.registrationState.collectAsState()
    val regReason by viewModel.regReason.collectAsState()
    val account = viewModel.accountStore.account
    val focusManager = LocalFocusManager.current
    var isUriInputMode by remember { mutableStateOf(false) }

    val statusColor = when (regState) {
        SipRegistrationState.REGISTERED -> IosGreen
        SipRegistrationState.REGISTERING -> IosOrange
        SipRegistrationState.FAILED -> IosRed
        SipRegistrationState.UNREGISTERED -> IosGray
    }

    val statusLabel = when (regState) {
        SipRegistrationState.REGISTERED -> "Online"
        SipRegistrationState.REGISTERING -> "Connecting"
        SipRegistrationState.FAILED -> "Failed"
        SipRegistrationState.UNREGISTERED -> "Offline"
    }

    val accountLine = when (regState) {
        SipRegistrationState.FAILED -> regReason.ifBlank { "${account.username} · ${account.domain}" }
        else -> "${account.username} · ${account.domain} · ${account.transport.uppercase()}"
    }

    val dialRows = remember {
        listOf(
            listOf("1" to "", "2" to "ABC", "3" to "DEF"),
            listOf("4" to "GHI", "5" to "JKL", "6" to "MNO"),
            listOf("7" to "PQRS", "8" to "TUV", "9" to "WXYZ"),
            listOf("*" to "", "0" to "+", "#" to "")
        )
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .statusBarsPadding()
            .padding(horizontal = 24.dp, vertical = 14.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.SpaceBetween
    ) {
        // Minimal Header: Identity + Single Live Status Pill (taps to Settings tab)
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                Text(
                    text = "SipSdk",
                    fontSize = 24.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = (-0.5).sp,
                    color = MaterialTheme.colorScheme.onBackground
                )
                Text(
                    text = accountLine,
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            Surface(
                modifier = Modifier
                    .clip(RoundedCornerShape(20.dp))
                    .clickable { viewModel.setSelectedTab(2) },
                shape = RoundedCornerShape(20.dp),
                color = statusColor.copy(alpha = 0.12f),
                border = BorderStroke(1.dp, statusColor.copy(alpha = 0.25f))
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(8.dp)
                            .clip(CircleShape)
                            .background(statusColor)
                    )
                    Text(
                        text = statusLabel,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = statusColor
                    )
                }
            }
        }

        // Number Display + Quick-Dial Strip
        Column(
            modifier = Modifier.fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            if (isUriInputMode) {
                OutlinedTextField(
                    value = dialDestination,
                    onValueChange = { viewModel.setDialDestination(it) },
                    modifier = Modifier.fillMaxWidth(),
                    textStyle = TextStyle(
                        fontSize = 20.sp,
                        fontWeight = FontWeight.SemiBold,
                        color = MaterialTheme.colorScheme.onSurface
                    ),
                    placeholder = {
                        Text(
                            text = "sip:user@domain",
                            fontSize = 17.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                        )
                    },
                    trailingIcon = {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (dialDestination.isNotEmpty()) {
                                IconButton(onClick = { viewModel.setDialDestination("") }) {
                                    Icon(
                                        imageVector = Icons.Filled.Close,
                                        contentDescription = "Clear",
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        modifier = Modifier.size(18.dp)
                                    )
                                }
                            }
                            IconButton(onClick = {
                                focusManager.clearFocus()
                                isUriInputMode = false
                            }) {
                                Icon(
                                    imageVector = Icons.Filled.Dialpad,
                                    contentDescription = "Switch to Keypad",
                                    tint = IosBlue,
                                    modifier = Modifier.size(20.dp)
                                )
                            }
                        }
                    },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(
                        keyboardType = KeyboardType.Email,
                        imeAction = ImeAction.Done
                    ),
                    keyboardActions = KeyboardActions(
                        onDone = { focusManager.clearFocus() }
                    ),
                    shape = RoundedCornerShape(16.dp),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedContainerColor = MaterialTheme.colorScheme.surface,
                        unfocusedContainerColor = MaterialTheme.colorScheme.surface,
                        focusedBorderColor = IosBlue,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline
                    )
                )
            } else {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(54.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = dialDestination.ifBlank { "Enter number" },
                        fontSize = if (dialDestination.length > 14) 24.sp else 34.sp,
                        fontWeight = if (dialDestination.isBlank()) FontWeight.Normal else FontWeight.SemiBold,
                        letterSpacing = 1.sp,
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        color = if (dialDestination.isBlank()) {
                            MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.45f)
                        } else {
                            MaterialTheme.colorScheme.onBackground
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 44.dp)
                    )

                    IconButton(
                        onClick = { isUriInputMode = true },
                        modifier = Modifier.align(Alignment.CenterEnd)
                    ) {
                        Icon(
                            imageVector = Icons.Filled.Keyboard,
                            contentDescription = "Enter SIP URI",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(20.dp)
                        )
                    }
                }
            }

            // Compact Quick-Dial Strip
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                listOf(
                    "9001" to "Echo",
                    "9002" to "Music",
                    "1002" to "Web"
                ).forEach { (ext, label) ->
                    val selected = dialDestination == ext
                    Surface(
                        modifier = Modifier
                            .clip(RoundedCornerShape(18.dp))
                            .clickable { viewModel.setDialDestination(ext) },
                        shape = RoundedCornerShape(18.dp),
                        color = if (selected) IosBlue.copy(alpha = 0.12f) else MaterialTheme.colorScheme.surface,
                        border = BorderStroke(
                            1.dp,
                            if (selected) IosBlue.copy(alpha = 0.35f) else MaterialTheme.colorScheme.outline
                        )
                    ) {
                        Text(
                            text = "$label · $ext",
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Medium,
                            maxLines = 1,
                            color = if (selected) IosBlue else MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp)
                        )
                    }
                }
            }
        }

        // Tactile 3x4 Numeric Keypad
        if (!isUriInputMode) {
            Column(
                modifier = Modifier.fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(14.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                dialRows.forEach { row ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceEvenly
                    ) {
                        row.forEach { (digit, letters) ->
                            DialpadKey(
                                digit = digit,
                                letters = letters,
                                onClick = { viewModel.setDialDestination(dialDestination + digit) },
                                onLongClick = if (digit == "0") {
                                    { viewModel.setDialDestination("$dialDestination+") }
                                } else {
                                    null
                                }
                            )
                        }
                    }
                }
            }
        } else {
            Spacer(modifier = Modifier.weight(1f))
        }

        // Primary Call Action Row (Clear Visual Hierarchy: Secondary Video | Primary Audio | Backspace)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(bottom = 8.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically
        ) {
            // Secondary Tonal Video Call Button
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Surface(
                    modifier = Modifier
                        .size(56.dp)
                        .clip(CircleShape)
                        .clickable(enabled = dialDestination.isNotBlank()) {
                            focusManager.clearFocus()
                            viewModel.makeCall(dialDestination, withVideo = true)
                        },
                    shape = CircleShape,
                    color = if (dialDestination.isNotBlank()) {
                        IosBlue.copy(alpha = 0.12f)
                    } else {
                        MaterialTheme.colorScheme.surfaceVariant
                    }
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(
                            imageVector = Icons.Filled.Videocam,
                            contentDescription = "Video Call",
                            tint = if (dialDestination.isNotBlank()) {
                                IosBlue
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f)
                            },
                            modifier = Modifier.size(24.dp)
                        )
                    }
                }
                Text(
                    text = "Video",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            // Primary CTA: Audio Call Button
            Surface(
                modifier = Modifier
                    .size(72.dp)
                    .shadow(
                        elevation = if (dialDestination.isNotBlank()) 8.dp else 0.dp,
                        shape = CircleShape,
                        ambientColor = IosGreen,
                        spotColor = IosGreen
                    )
                    .clip(CircleShape)
                    .clickable(enabled = dialDestination.isNotBlank()) {
                        focusManager.clearFocus()
                        viewModel.makeCall(dialDestination, withVideo = false)
                    },
                shape = CircleShape,
                color = if (dialDestination.isNotBlank()) IosGreen else IosGreen.copy(alpha = 0.35f)
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Icon(
                        imageVector = Icons.Filled.Phone,
                        contentDescription = "Audio Call",
                        tint = Color.White,
                        modifier = Modifier.size(30.dp)
                    )
                }
            }

            // Utility: Backspace (tap deletes 1 char, long-press clears)
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(56.dp)
                        .clip(CircleShape)
                        .combinedClickable(
                            enabled = dialDestination.isNotEmpty(),
                            onClick = {
                                if (dialDestination.isNotEmpty()) {
                                    viewModel.setDialDestination(dialDestination.dropLast(1))
                                }
                            },
                            onLongClick = { viewModel.setDialDestination("") }
                        ),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Filled.Backspace,
                        contentDescription = "Delete",
                        tint = if (dialDestination.isNotEmpty()) {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f)
                        },
                        modifier = Modifier.size(24.dp)
                    )
                }
                Text(
                    text = "Clear",
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Medium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(
                        alpha = if (dialDestination.isNotEmpty()) 1f else 0.35f
                    )
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun DialpadKey(
    digit: String,
    letters: String,
    onClick: () -> Unit,
    onLongClick: (() -> Unit)? = null
) {
    Surface(
        modifier = Modifier
            .size(72.dp)
            .clip(CircleShape)
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick
            ),
        shape = CircleShape,
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.8f))
    ) {
        Column(
            modifier = Modifier.fillMaxSize(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = digit,
                fontSize = 27.sp,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
                lineHeight = 29.sp
            )
            if (letters.isNotEmpty()) {
                Text(
                    text = letters,
                    fontSize = 10.sp,
                    fontWeight = FontWeight.SemiBold,
                    letterSpacing = 1.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}
