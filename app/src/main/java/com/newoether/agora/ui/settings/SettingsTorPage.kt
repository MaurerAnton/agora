package com.newoether.agora.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.viewmodel.ChatViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsTorPage(viewModel: ChatViewModel, onBack: () -> Unit) {
    val torEnabled by viewModel.torEnabled.collectAsState()
    val torSocksPort by viewModel.torSocksPort.collectAsState()
    val showDocFab by viewModel.showDocumentationFab.collectAsState()
    var portText by remember(torSocksPort) { mutableStateOf(torSocksPort.toString()) }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        contentWindowInsets = WindowInsets(0.dp),
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.tor_title), fontWeight = FontWeight.Bold) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.back))
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                    titleContentColor = MaterialTheme.colorScheme.onBackground,
                )
            )
        }
    ) { padding ->
        val fm = androidx.compose.ui.platform.LocalFocusManager.current
        val scrollState = rememberScrollState()
        Column(
            modifier = Modifier
                .padding(padding)
                .navigationBarsPadding()
                .imePadding()
                .verticalScroll(scrollState)
                .clickable(indication = null, interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() }) { fm.clearFocus() }
                .padding(horizontal = 16.dp, vertical = 16.dp)
        ) {
            SettingsGroup(title = stringResource(R.string.tor_title), items = listOf({
                SettingsItem(
                    headlineContent = { Text(stringResource(R.string.tor_enable)) },
                    supportingContent = { Text(stringResource(R.string.tor_enable_desc)) },
                    leadingContent = { Icon(Icons.Default.Security, null, tint = MaterialTheme.colorScheme.primary) },
                    trailingContent = {
                        Switch(checked = torEnabled, onCheckedChange = { viewModel.setTorEnabled(it) })
                    },
                    modifier = Modifier.clickable { viewModel.setTorEnabled(!torEnabled) }
                )
            }))

            if (torEnabled) {
                Spacer(modifier = Modifier.height(16.dp))

                SettingsGroup(title = stringResource(R.string.tor_socks_settings), items = listOf({
                    SettingsItem(
                        headlineContent = { Text(stringResource(R.string.tor_socks_port)) },
                        supportingContent = { Text(stringResource(R.string.tor_socks_port_desc)) },
                        leadingContent = { Icon(Icons.Default.SettingsEthernet, null, tint = MaterialTheme.colorScheme.primary) },
                        trailingContent = {
                            OutlinedTextField(
                                value = portText,
                                onValueChange = { newValue ->
                                    val filtered = newValue.filter { it.isDigit() }
                                    portText = filtered
                                    val port = filtered.toIntOrNull()
                                    if (port != null && port in 1..65535) {
                                        viewModel.setTorSocksPort(port)
                                    }
                                },
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                                singleLine = true,
                                shape = RoundedCornerShape(12.dp),
                                modifier = Modifier.width(100.dp)
                            )
                        }
                    )
                }))

                Spacer(modifier = Modifier.height(16.dp))

                Card(
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
                    ),
                    shape = RoundedCornerShape(16.dp),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Row(
                        modifier = Modifier.padding(16.dp),
                        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically
                    ) {
                        Icon(
                            Icons.Default.Info,
                            null,
                            tint = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.size(20.dp)
                        )
                        Spacer(modifier = Modifier.width(12.dp))
                        Text(
                            stringResource(R.string.tor_info),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
    }
}
