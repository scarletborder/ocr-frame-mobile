package ai.ocrframe.vlocr.ui

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import ai.ocrframe.vlocr.EngineHost
import ai.ocrframe.vlocr.ModelStore
import ai.ocrframe.vlocr.VlEngine
import kotlinx.coroutines.launch

@Composable
fun SettingsScreen(store: ModelStore, host: EngineHost) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var error by remember { mutableStateOf<String?>(null) }
    var remoteDraft by remember { mutableStateOf(store.remoteBaseUrl) }

    fun importModel(uri: Uri?, slot: ModelStore.Slot) {
        if (uri == null) return
        runCatching {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        scope.launch {
            runCatching { store.import(uri, slot) }
                .onFailure { error = "导入失败：${it.message}" }
            host.invalidate()
        }
    }

    val layoutPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) {
        importModel(it, ModelStore.Slot.LAYOUT)
    }
    val vlmPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) {
        importModel(it, ModelStore.Slot.VLM)
    }
    val mmprojPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            scope.launch {
                runCatching { store.importMmproj(uri) }
                    .onFailure { error = "导入失败：${it.message}" }
                host.invalidate()
            }
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("预设（写死为当前 VL 线做法）", style = MaterialTheme.typography.titleSmall)
                Text(
                    "${VlEngine.presetName()} ${VlEngine.presetVersion()} · 产线 ${VlEngine.pipelineVersion()}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    "生成长度上限 ${VlEngine.maxNewTokens()} tokens · 参数由共享核心 impl/shard 提供",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column {
                Text(
                    "模型",
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.padding(start = 16.dp, top = 16.dp, end = 16.dp),
                )
                ModelRow(
                    title = "版面分析模型",
                    subtitle = "PP-DocLayoutV3（.onnx）",
                    selected = store.layoutName,
                ) { layoutPicker.launch(arrayOf("*/*")) }
                HorizontalDivider()
                ModelRow(
                    title = "视觉语言识别模型",
                    subtitle = "PaddleOCR-VL-1.6（.gguf）",
                    selected = store.vlmName,
                ) { vlmPicker.launch(arrayOf("*/*")) }
                HorizontalDivider()
                ModelRow(
                    title = "视觉投影器",
                    subtitle = "*-mmproj.gguf（与识别模型成对）",
                    selected = store.mmprojName,
                ) { mmprojPicker.launch(arrayOf("*/*")) }
                Text(
                    "VL 线必须两个模型成对使用；选完主权重后会自动在同目录找 mmproj。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(16.dp),
                )
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("运行方式", style = MaterialTheme.typography.titleSmall)
                ModelStore.Mode.entries.forEach { mode ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.fillMaxWidth().clickable {
                            store.setMode(mode)
                            host.invalidate()
                        },
                    ) {
                        RadioButton(
                            selected = store.mode == mode,
                            onClick = {
                                store.setMode(mode)
                                host.invalidate()
                            },
                        )
                        Text(modeTitle(mode), style = MaterialTheme.typography.bodyMedium)
                    }
                }
                if (store.mode == ModelStore.Mode.REMOTE) {
                    OutlinedTextField(
                        value = remoteDraft,
                        onValueChange = { remoteDraft = it },
                        label = { Text("服务地址（含 /v1）") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Text(
                        "保存地址",
                        color = MaterialTheme.colorScheme.primary,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier.clickable {
                            store.setRemoteBaseUrl(remoteDraft)
                            host.invalidate()
                        },
                    )
                }
                Text(
                    "默认在设备内跑 GGUF；远程模式用于未编译 llama.cpp 时验证整条链路。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("引擎", style = MaterialTheme.typography.titleSmall)
                Text(
                    "版面分析引擎：${if (VlEngine.layoutEngineBuilt()) "已编译" else "未编译"}",
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "VLM 引擎：${if (VlEngine.vlmEngineBuilt()) "已编译" else "未编译"}",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        error?.let { message ->
            Text(message, color = Color(0xFFB3261E), style = MaterialTheme.typography.bodySmall)
        }

        Text(
            "本 demo 只实现「选图 → 一次性识别」，不做保存、项目与校对。模型文件放在 App 私有目录 files/models 下。",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

private fun modeTitle(mode: ModelStore.Mode): String = when (mode) {
    ModelStore.Mode.LOCAL -> "本地 GGUF（llama.cpp）"
    ModelStore.Mode.REMOTE -> "远程服务（OpenAI 兼容）"
}

@Composable
private fun ModelRow(
    title: String,
    subtitle: String,
    selected: String?,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick).padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                text = selected ?: "未选择",
                style = MaterialTheme.typography.bodySmall,
                color = if (selected == null) Color(0xFFE65100) else MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
        Text("选择", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.labelLarge)
    }
}
