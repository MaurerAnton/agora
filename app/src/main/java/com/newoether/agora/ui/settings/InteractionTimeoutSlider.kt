package com.newoether.agora.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Timer
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.data.ASK_USER_TIMEOUT_PRESETS
import com.newoether.agora.data.normalizeAskUserTimeoutMinutes
import com.newoether.agora.ui.common.PersistedSliderFeedbackGate
import kotlin.math.roundToInt

@Composable
internal fun InteractionTimeoutSlider(
    value: Int,
    onValueChange: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val presets = ASK_USER_TIMEOUT_PRESETS
    val persistedValue = normalizeAskUserTimeoutMinutes(value)
    val gate = remember {
        PersistedSliderFeedbackGate(
            initialPersisted = persistedValue,
            toDisplay = { presets.indexOf(it).toFloat() },
        )
    }
    LaunchedEffect(persistedValue) { gate.reconcile(persistedValue) }
    val selectedIndex = gate.displayed.roundToInt().coerceIn(presets.indices)
    val title = stringResource(R.string.ask_user_timeout)

    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 16.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
            Icon(
                Icons.Outlined.Timer,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(top = 2.dp),
            )
            Spacer(Modifier.width(16.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = title,
                        style = MaterialTheme.typography.bodyLarge.copy(fontWeight = FontWeight.Medium),
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        text = stringResource(R.string.ask_user_timeout_minutes, presets[selectedIndex]),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.primary,
                        fontWeight = FontWeight.Bold,
                    )
                }
                Text(
                    text = stringResource(R.string.ask_user_timeout_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 4.dp),
                )
                Slider(
                    value = gate.displayed,
                    onValueChange = gate::updateFromGesture,
                    onValueChangeFinished = {
                        val index = gate.displayed.roundToInt().coerceIn(presets.indices)
                        val minutes = presets[index]
                        if (minutes != persistedValue) {
                            gate.expectPersisted(minutes, index.toFloat())
                            onValueChange(minutes)
                        } else {
                            gate.settleWithoutWrite(persistedValue, index.toFloat())
                        }
                    },
                    valueRange = 0f..presets.lastIndex.toFloat(),
                    steps = presets.size - 2,
                    modifier = modifier.fillMaxWidth().padding(top = 8.dp)
                        .semantics { contentDescription = title },
                )
            }
        }
    }
}
