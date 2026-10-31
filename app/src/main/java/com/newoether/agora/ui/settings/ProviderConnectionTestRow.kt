package com.newoether.agora.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.api.LlmProvider
import com.newoether.agora.api.anthropic.AnthropicProvider
import com.newoether.agora.api.gemini.GeminiProvider
import com.newoether.agora.api.ollama.OllamaProvider
import com.newoether.agora.data.CustomEndpointProtocol
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.viewmodel.ChatViewModel
import com.newoether.agora.viewmodel.ProbeWire
import com.newoether.agora.viewmodel.ProviderProbeReport
import com.newoether.agora.viewmodel.connectionProbeSteps
import com.newoether.agora.viewmodel.render
import com.newoether.agora.viewmodel.runConnectionProbe
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/**
 * Connection test for the settings "Connection" group: issues the same requests a real
 * conversation would (model list, plus a one-token chat turn) and reports the exact endpoint,
 * HTTP status and provider detail for each attempt, so a wrong Base URL or a missing /v1 is
 * visible without a debugger. Renders nothing for on-device models or an unknown protocol.
 */
@Composable
internal fun ProviderConnectionTestRow(
    providerName: String,
    viewModel: ChatViewModel,
    modifier: Modifier = Modifier,
) {
    val customProviders by viewModel.settings.customProviders.collectAsState()
    val providerBaseUrls by viewModel.settings.providerBaseUrls.collectAsState()
    val apiKeys by viewModel.settings.apiKeys.collectAsState()
    val activeApiKeyIds by viewModel.settings.activeApiKeyIds.collectAsState()
    val availableModels by viewModel.settings.availableModels.collectAsState()

    val config = customProviders.firstOrNull { it.name == providerName }
    val provider = viewModel.getProviderInstanceOrNull(providerName)
    val wire = probeWire(provider, config)
    val baseUrl = providerBaseUrls[providerName]?.takeIf(String::isNotBlank)
        ?: provider?.defaultBaseUrl?.takeIf(String::isNotBlank)
    if (wire == null || baseUrl == null) return

    val scope = rememberCoroutineScope()
    var running by remember(providerName) { mutableStateOf(false) }
    var report by remember(providerName) { mutableStateOf<ProviderProbeReport?>(null) }

    OutlinedButton(
        enabled = !running,
        onClick = {
            val apiKey = apiKeys.firstOrNull { it.id == activeApiKeyIds[providerName] }?.key.orEmpty()
            val steps = connectionProbeSteps(
                wire = wire,
                baseUrl = baseUrl,
                apiKey = apiKey,
                chatModel = availableModels[providerName]?.firstOrNull(),
            )
            if (steps.isEmpty()) return@OutlinedButton
            running = true
            scope.launch {
                report = try {
                    runConnectionProbe(steps)
                } catch (error: CancellationException) {
                    throw error
                } catch (error: Exception) {
                    ProviderProbeReport(attempts = emptyList())
                }
                running = false
            }
        },
        modifier = modifier.fillMaxWidth(),
    ) {
        Text(
            stringResource(
                if (running) {
                    R.string.provider_connection_test_running
                } else {
                    R.string.provider_connection_test
                },
            ),
        )
    }

    report?.let { value -> ConnectionTestReportDialog(value) { report = null } }
}

/** Wire shape of a provider instance, or null when this provider has no probe (unknown/custom). */
private fun probeWire(provider: LlmProvider?, config: CustomProviderConfig?): ProbeWire? {
    if (config != null) {
        return when (config.protocol) {
            CustomEndpointProtocol.OPENAI -> ProbeWire.OPENAI
            CustomEndpointProtocol.ANTHROPIC -> ProbeWire.ANTHROPIC
            CustomEndpointProtocol.GOOGLE -> ProbeWire.GOOGLE
            CustomEndpointProtocol.UNKNOWN -> null
        }
    }
    return when (provider) {
        is AnthropicProvider -> ProbeWire.ANTHROPIC
        is GeminiProvider -> ProbeWire.GOOGLE
        is OllamaProvider -> ProbeWire.OLLAMA
        null -> null
        // OpenAI-compatible built-ins and the debug fixture share the OpenAI wire.
        else -> ProbeWire.OPENAI
    }
}

@Composable
private fun ConnectionTestReportDialog(
    report: ProviderProbeReport,
    onDismiss: () -> Unit,
) {
    val clipboard = LocalClipboardManager.current
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        title = {
            Text(
                stringResource(R.string.provider_connection_test_title),
                fontWeight = FontWeight.Bold,
            )
        },
        text = {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(
                    text = stringResource(
                        if (report.successful) {
                            R.string.provider_connection_test_ok
                        } else {
                            R.string.provider_connection_test_failed
                        },
                    ),
                    color = if (report.successful) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.error
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                )
                Spacer(modifier = Modifier.height(8.dp))
                report.attempts.forEach { attempt ->
                    Text(
                        text = attempt.render(),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(bottom = 4.dp),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.ok)) }
        },
        dismissButton = {
            TextButton(
                onClick = { clipboard.setText(AnnotatedString(report.render())) },
            ) {
                Text(stringResource(R.string.copy))
            }
        },
    )
}
