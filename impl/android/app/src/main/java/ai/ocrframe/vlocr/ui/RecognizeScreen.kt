package ai.ocrframe.vlocr.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.PhotoLibrary
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import ai.ocrframe.vlocr.EngineHost
import ai.ocrframe.vlocr.ModelStore
import ai.ocrframe.vlocr.PageResult
import ai.ocrframe.vlocr.RecognizeViewModel
import ai.ocrframe.vlocr.VlEngine
import ai.ocrframe.vlocr.decodeBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

@Composable
fun RecognizeScreen(store: ModelStore, host: EngineHost) {
    val context = LocalContext.current
    val viewModel: RecognizeViewModel = viewModel()
    val scope = rememberCoroutineScope()

    val items by viewModel.items.collectAsStateWithLifecycle()
    val running by viewModel.running.collectAsStateWithLifecycle()
    val error by viewModel.error.collectAsStateWithLifecycle()

    var detail by remember { mutableStateOf<PageResult?>(null) }
    var localError by remember { mutableStateOf<String?>(null) }
    val cameraUri = remember { mutableStateOf<Uri?>(null) }

    val cameraLauncher = rememberLauncherForActivityResult(ActivityResultContracts.TakePicture()) { success ->
        val uri = cameraUri.value
        if (success && uri != null) {
            scope.launch {
                val bitmap = withContext(Dispatchers.IO) { decodeBitmap(context, uri) }
                if (bitmap == null) {
                    localError = "照片解码失败。"
                } else {
                    viewModel.recognize(listOf(bitmap), store, host)
                }
            }
        }
    }

    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            launchCamera(context, cameraLauncher, cameraUri)
        } else {
            localError = "没有相机权限，请在系统设置里允许后重试。"
        }
    }

    val pickImages = rememberLauncherForActivityResult(ActivityResultContracts.PickMultipleVisualMedia(20)) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            val bitmaps = withContext(Dispatchers.IO) { uris.mapNotNull { decodeBitmap(context, it) } }
            if (bitmaps.isEmpty()) {
                localError = "选中的图片无法读取。"
            } else {
                viewModel.recognize(bitmaps, store, host)
            }
        }
    }

    val pickFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val bitmap = withContext(Dispatchers.IO) { decodeBitmap(context, uri) }
            if (bitmap == null) {
                localError = "选中的文件不是可识别的图片。"
            } else {
                viewModel.recognize(listOf(bitmap), store, host)
            }
        }
    }

    detail?.let { result ->
        PageDetailScreen(result) { detail = null }
        return
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        StatusCard(store)

        Box(
            modifier = Modifier.fillMaxWidth(),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier
                    .size(200.dp)
                    .clip(CircleShape)
                    .background(
                        Brush.linearGradient(
                            listOf(MaterialTheme.colorScheme.primary, MaterialTheme.colorScheme.primary.copy(alpha = 0.72f)),
                        ),
                    )
                    .clickable(enabled = !running) {
                        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                            PackageManager.PERMISSION_GRANTED
                        ) {
                            launchCamera(context, cameraLauncher, cameraUri)
                        } else {
                            permissionLauncher.launch(Manifest.permission.CAMERA)
                        }
                    },
                verticalArrangement = Arrangement.Center,
            ) {
                Icon(
                    Icons.Filled.CameraAlt,
                    contentDescription = null,
                    tint = Color.White,
                    modifier = Modifier.size(56.dp),
                )
                Text("拍摄识别", color = Color.White, fontWeight = FontWeight.SemiBold)
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            EntryButton(
                title = "从相册选择",
                icon = Icons.Filled.PhotoLibrary,
                modifier = Modifier.weight(1f),
            ) {
                pickImages.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
            }
            EntryButton(
                title = "从文件选择",
                icon = Icons.Filled.Folder,
                modifier = Modifier.weight(1f),
            ) {
                pickFiles.launch(arrayOf("image/*"))
            }
        }

        (error ?: localError)?.let { message ->
            Card(colors = CardDefaults.cardColors(containerColor = Color(0x14FF3B30))) {
                Text(
                    text = message,
                    color = Color(0xFFB3261E),
                    modifier = Modifier.padding(12.dp),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        items.forEach { item ->
            ItemRow(item) { result -> detail = result }
        }
    }
}

@Composable
private fun StatusCard(store: ModelStore) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                text = if (store.isReady) "模型已就绪" else "模型未就绪",
                style = MaterialTheme.typography.titleMedium,
                color = if (store.isReady) Color(0xFF2E7D32) else Color(0xFFE65100),
            )
            Text(
                text = "预设：${VlEngine.presetName()} ${VlEngine.presetVersion()}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = "产线 ${VlEngine.pipelineVersion()} · ${modeTitle(store)}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (!store.isReady) {
                Text(
                    text = "请到「设置」里选择版面分析模型与视觉语言识别模型。",
                    style = MaterialTheme.typography.bodySmall,
                    color = Color(0xFFE65100),
                )
            }
        }
    }
}

private fun modeTitle(store: ModelStore): String = when (store.mode) {
    ModelStore.Mode.LOCAL -> "本地 GGUF（llama.cpp）"
    ModelStore.Mode.REMOTE -> "远程服务（OpenAI 兼容）"
}

@Composable
private fun EntryButton(
    title: String,
    icon: ImageVector,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Card(modifier = modifier.clickable(onClick = onClick)) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(vertical = 14.dp),
            horizontalArrangement = Arrangement.Center,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(18.dp))
            Text("  $title", style = MaterialTheme.typography.labelLarge)
        }
    }
}

@Composable
private fun ItemRow(item: RecognizeViewModel.Item, onOpen: (PageResult) -> Unit) {
    val status = item.status
    val done = status is RecognizeViewModel.Status.Done

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(enabled = done) {
                if (status is RecognizeViewModel.Status.Done) onOpen(status.result)
            },
    ) {
        Row(
            modifier = Modifier.padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            androidx.compose.foundation.Image(
                bitmap = item.thumbnail.asImageBitmap(),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.size(56.dp, 72.dp).clip(RoundedCornerShape(8.dp)),
            )
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title(status), style = MaterialTheme.typography.titleSmall)
                Text(
                    text = detailText(status),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (status is RecognizeViewModel.Status.Running && status.total > 0) {
                LinearProgressIndicator(
                    progress = { status.done.toFloat() / status.total.toFloat() },
                    modifier = Modifier.size(56.dp, 6.dp),
                )
            }
        }
    }
}

private fun title(status: RecognizeViewModel.Status): String = when (status) {
    RecognizeViewModel.Status.Pending -> "等待识别"
    is RecognizeViewModel.Status.Running -> "识别中…"
    is RecognizeViewModel.Status.Done -> "识别完成"
    is RecognizeViewModel.Status.Failed -> "识别失败"
}

private fun detailText(status: RecognizeViewModel.Status): String = when (status) {
    RecognizeViewModel.Status.Pending -> "排队中"
    is RecognizeViewModel.Status.Running ->
        if (status.total > 0) "已处理 ${status.done}/${status.total} 个版面块" else "正在分析版面…"
    is RecognizeViewModel.Status.Done ->
        "%.1f 秒 · %d 个版面块".format(status.result.seconds, status.result.blocks.size)
    is RecognizeViewModel.Status.Failed -> status.message
}

private fun launchCamera(
    context: Context,
    launcher: ActivityResultLauncher<Uri>,
    holder: MutableState<Uri?>,
) {
    val directory = File(context.cacheDir, "camera").apply { mkdirs() }
    val file = File(directory, "capture-${System.currentTimeMillis()}.jpg")
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
    holder.value = uri
    launcher.launch(uri)
}
