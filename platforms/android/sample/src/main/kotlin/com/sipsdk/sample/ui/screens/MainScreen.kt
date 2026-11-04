package com.sipsdk.sample.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Dialpad
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.sipsdk.sample.ui.theme.IosBlue
import com.sipsdk.sample.ui.theme.IosLightSeparator
import com.sipsdk.sample.viewmodels.SipAppViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(
    viewModel: SipAppViewModel,
    modifier: Modifier = Modifier
) {
    val selectedTab by viewModel.selectedTab.collectAsState()
    val isShowingAccountSheet by viewModel.isShowingAccountSheet.collectAsState()
    val isShowingInCallView by viewModel.isShowingInCallView.collectAsState()
    val incomingCall by viewModel.incomingCall.collectAsState()

    Box(modifier = modifier.fillMaxSize()) {
        if (!isShowingInCallView) {
            Scaffold(
                bottomBar = {
                    NavigationBar(
                        containerColor = MaterialTheme.colorScheme.surface,
                        tonalElevation = 8.dp
                    ) {
                        NavigationBarItem(
                            selected = selectedTab == 0,
                            onClick = { viewModel.setSelectedTab(0) },
                            icon = {
                                Icon(
                                    imageVector = Icons.Filled.Dialpad,
                                    contentDescription = "Keypad"
                                )
                            },
                            label = {
                                Text(
                                    text = "Keypad",
                                    fontSize = 11.sp,
                                    fontWeight = if (selectedTab == 0) FontWeight.SemiBold else FontWeight.Normal
                                )
                            },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = IosBlue,
                                selectedTextColor = IosBlue,
                                unselectedIconColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                unselectedTextColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                indicatorColor = Color.Transparent
                            )
                        )

                        NavigationBarItem(
                            selected = selectedTab == 1,
                            onClick = { viewModel.setSelectedTab(1) },
                            icon = {
                                Icon(
                                    imageVector = Icons.Filled.History,
                                    contentDescription = "Recents"
                                )
                            },
                            label = {
                                Text(
                                    text = "Recents",
                                    fontSize = 11.sp,
                                    fontWeight = if (selectedTab == 1) FontWeight.SemiBold else FontWeight.Normal
                                )
                            },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = IosBlue,
                                selectedTextColor = IosBlue,
                                unselectedIconColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                unselectedTextColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                indicatorColor = Color.Transparent
                            )
                        )

                        NavigationBarItem(
                            selected = selectedTab == 2,
                            onClick = { viewModel.setSelectedTab(2) },
                            icon = {
                                Icon(
                                    imageVector = Icons.Filled.Settings,
                                    contentDescription = "Settings"
                                )
                            },
                            label = {
                                Text(
                                    text = "Settings",
                                    fontSize = 11.sp,
                                    fontWeight = if (selectedTab == 2) FontWeight.SemiBold else FontWeight.Normal
                                )
                            },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = IosBlue,
                                selectedTextColor = IosBlue,
                                unselectedIconColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                unselectedTextColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                indicatorColor = Color.Transparent
                            )
                        )
                    }
                }
            ) { innerPadding ->
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(innerPadding)
                ) {
                    when (selectedTab) {
                        0 -> HomeScreen(viewModel = viewModel)
                        1 -> CallHistoryScreen(viewModel = viewModel)
                        2 -> SettingsScreen(
                            viewModel = viewModel,
                            onDismiss = { viewModel.setSelectedTab(0) }
                        )
                    }
                }
            }

            // Fullscreen sheet for Settings if triggered from Home status badge / gear icon
            if (isShowingAccountSheet) {
                Box(modifier = Modifier.fillMaxSize()) {
                    SettingsScreen(
                        viewModel = viewModel,
                        onDismiss = { viewModel.setShowAccountSheet(false) }
                    )
                }
            }

            // Fullscreen overlay for Incoming Call
            AnimatedVisibility(
                visible = incomingCall != null,
                enter = fadeIn(),
                exit = fadeOut()
            ) {
                IncomingCallScreen(viewModel = viewModel)
            }
        } else {
            InCallScreen(viewModel = viewModel)
        }
    }
}
