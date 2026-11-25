package ai.ocrframe.vlocr.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val VLColors = lightColorScheme(
    primary = Color(0xFF2F6BFF),
    secondary = Color(0xFF4C6EF5),
    background = Color(0xFFF6F7FB),
    surface = Color(0xFFFFFFFF),
)

@Composable
fun VLOCRTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = VLColors, content = content)
}
