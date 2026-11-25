package ai.ocrframe.vlocr

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import ai.ocrframe.vlocr.ui.AppRoot
import ai.ocrframe.vlocr.ui.VLOCRTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            VLOCRTheme {
                AppRoot()
            }
        }
    }
}
