package com.newoether.agora.ui.settings

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

private val PillTabCornerVisibilityThreshold = 0.01.dp
private const val PillTabWeightVisibilityThreshold = 0.001f

/**
 * Expressive segmented "pill" tab switcher: a row of equal-weight tabs where the selected tab
 * springs to a full-pill shape, grows slightly wider (bouncy), and fills with the primary color,
 * while its neighbours square off to an 8dp inner corner. Originally hand-designed inline for the
 * system prompt editor; extracted here so any settings surface can reuse the identical motion.
 *
 * @param tabs the labels, left to right.
 * @param selectedIndex currently selected tab.
 * @param onSelect invoked with the tapped tab index.
 * @param tabHeight pill height; the selected/edge corner radius is half of this.
 * @param enabled whether the tabs accept input.
 * @param wrapLabels whether labels wrap and share a natural row height for narrow/large-font layouts.
 * @param allowLabelOverflow whether a single-line label may paint beyond its measured bounds
 * instead of being truncated with an ellipsis.
 */
@Composable
fun PillTabSwitcher(
    tabs: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    tabHeight: Dp = 44.dp,
    allowLabelOverflow: Boolean = false,
    enabled: Boolean = true,
    wrapLabels: Boolean = false,
) {
    Row(
        modifier = modifier.fillMaxWidth().selectableGroup()
            .then(if (wrapLabels) Modifier.height(IntrinsicSize.Min) else Modifier),
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        tabs.forEachIndexed { index, label ->
            val isSelected = selectedIndex == index
            val outerCorner = tabHeight / 2f
            val innerCorner = 8.dp
            val targetTopStart = if (isSelected || index == 0) outerCorner else innerCorner
            val targetBottomStart = if (isSelected || index == 0) outerCorner else innerCorner
            val targetTopEnd = if (isSelected || index == tabs.lastIndex) outerCorner else innerCorner
            val targetBottomEnd = if (isSelected || index == tabs.lastIndex) outerCorner else innerCorner
            val cornerSpec = spring<Dp>(
                dampingRatio = Spring.DampingRatioHighBouncy,
                stiffness = Spring.StiffnessMediumLow,
                visibilityThreshold = PillTabCornerVisibilityThreshold
            )
            val topStart by animateDpAsState(targetTopStart, cornerSpec, label = "pillTabTopStart")
            val topEnd by animateDpAsState(targetTopEnd, cornerSpec, label = "pillTabTopEnd")
            val bottomStart by animateDpAsState(targetBottomStart, cornerSpec, label = "pillTabBottomStart")
            val bottomEnd by animateDpAsState(targetBottomEnd, cornerSpec, label = "pillTabBottomEnd")
            val safeTopStart = topStart.coerceIn(innerCorner, outerCorner)
            val safeTopEnd = topEnd.coerceIn(innerCorner, outerCorner)
            val safeBottomStart = bottomStart.coerceIn(innerCorner, outerCorner)
            val safeBottomEnd = bottomEnd.coerceIn(innerCorner, outerCorner)
            val containerColor by animateColorAsState(
                targetValue = if (isSelected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainerHigh,
                animationSpec = tween(220),
                label = "pillTabContainerColor"
            )
            val contentColor by animateColorAsState(
                targetValue = if (isSelected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                animationSpec = tween(220),
                label = "pillTabContentColor"
            )
            val widthWeight by animateFloatAsState(
                targetValue = if (isSelected) 1.13f else 1f,
                animationSpec = spring(
                    dampingRatio = Spring.DampingRatioHighBouncy,
                    stiffness = Spring.StiffnessMediumLow,
                    visibilityThreshold = PillTabWeightVisibilityThreshold
                ),
                label = "pillTabWeight"
            )
            Surface(
                onClick = { onSelect(index) },
                enabled = enabled,
                modifier = Modifier.weight(widthWeight)
                    .then(if (wrapLabels) Modifier.fillMaxHeight().heightIn(min = tabHeight) else Modifier.height(tabHeight))
                    .semantics { selected = isSelected; role = Role.Tab },
                shape = RoundedCornerShape(
                    topStart = safeTopStart,
                    topEnd = safeTopEnd,
                    bottomStart = safeBottomStart,
                    bottomEnd = safeBottomEnd
                ),
                color = containerColor,
                contentColor = contentColor
            ) {
                Box(
                    modifier = Modifier.fillMaxSize()
                        .padding(horizontal = 12.dp, vertical = if (wrapLabels) 8.dp else 0.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = label,
                        maxLines = if (wrapLabels) Int.MAX_VALUE else 1,
                        overflow = if (allowLabelOverflow && !wrapLabels) TextOverflow.Visible else TextOverflow.Ellipsis,
                        softWrap = wrapLabels,
                        textAlign = TextAlign.Center,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal
                    )
                }
            }
        }
    }
}
