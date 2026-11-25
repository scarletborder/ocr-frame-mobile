package ai.ocrframe.vlocr

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 两个模型槽位（对应后端 data/models/{vl_layout,vl_vlm}/）。
 *
 * Android 侧不用 security-scoped bookmark：选中的文件直接复制进 filesDir/models，
 * 之后 llama.cpp / ONNX Runtime 都能按普通路径 mmap。
 */
class ModelStore(private val context: Context) {

    enum class Slot { LAYOUT, VLM }

    enum class Mode { LOCAL, REMOTE }

    private val prefs = context.getSharedPreferences("vlocr", Context.MODE_PRIVATE)

    val modelsDir: File = File(context.filesDir, "models").apply { mkdirs() }

    var layoutName by mutableStateOf(prefs.getString(KEY_LAYOUT, null))
        private set
    var vlmName by mutableStateOf(prefs.getString(KEY_VLM, null))
        private set
    var mmprojName by mutableStateOf(prefs.getString(KEY_MMPROJ, null))
        private set
    var mode by mutableStateOf(
        runCatching { Mode.valueOf(prefs.getString(KEY_MODE, Mode.LOCAL.name)!!) }.getOrDefault(Mode.LOCAL)
    )
        private set
    var remoteBaseUrl by mutableStateOf(prefs.getString(KEY_REMOTE, "http://192.168.1.10:8118/v1")!!)
        private set

    val layoutFile: File? get() = existing(layoutName)
    val vlmFile: File? get() = existing(vlmName)
    val mmprojFile: File? get() = existing(mmprojName)

    val isReady: Boolean
        get() = when (mode) {
            Mode.LOCAL -> layoutFile != null && vlmFile != null && mmprojFile != null &&
                VlEngine.layoutEngineBuilt() && VlEngine.vlmEngineBuilt()
            Mode.REMOTE -> layoutFile != null && VlEngine.layoutEngineBuilt() && remoteBaseUrl.isNotBlank()
        }

    fun setMode(value: Mode) {
        mode = value
        prefs.edit().putString(KEY_MODE, value.name).apply()
    }

    fun setRemoteBaseUrl(value: String) {
        remoteBaseUrl = value
        prefs.edit().putString(KEY_REMOTE, value).apply()
    }

    suspend fun import(uri: Uri, slot: Slot) {
        val name = withContext(Dispatchers.IO) { copyIntoModels(uri) }
        when (slot) {
            Slot.LAYOUT -> {
                layoutName = name
                prefs.edit().putString(KEY_LAYOUT, name).apply()
            }
            Slot.VLM -> {
                vlmName = name
                prefs.edit().putString(KEY_VLM, name).apply()
                autoDetectMmproj()
            }
        }
    }

    suspend fun importMmproj(uri: Uri) {
        val name = withContext(Dispatchers.IO) { copyIntoModels(uri) }
        mmprojName = name
        prefs.edit().putString(KEY_MMPROJ, name).apply()
    }

    fun clear(slot: Slot) {
        when (slot) {
            Slot.LAYOUT -> {
                layoutName = null
                prefs.edit().remove(KEY_LAYOUT).apply()
            }
            Slot.VLM -> {
                vlmName = null
                mmprojName = null
                prefs.edit().remove(KEY_VLM).remove(KEY_MMPROJ).apply()
            }
        }
    }

    /** 视觉投影器是独立文件（*-mmproj.gguf），选完主权重后自动在同目录里找。 */
    private fun autoDetectMmproj() {
        val match = modelsDir.listFiles()?.firstOrNull { file ->
            val lower = file.name.lowercase()
            lower.endsWith(".gguf") && (lower.contains("mmproj") || lower.contains("mm_proj"))
        } ?: return
        mmprojName = match.name
        prefs.edit().putString(KEY_MMPROJ, match.name).apply()
    }

    private fun copyIntoModels(uri: Uri): String {
        val name = displayName(uri) ?: "model-${System.currentTimeMillis()}.bin"
        val target = File(modelsDir, name)
        val input = context.contentResolver.openInputStream(uri)
            ?: error("无法读取所选文件。")
        input.use { source ->
            target.outputStream().use { output -> source.copyTo(output) }
        }
        return name
    }

    private fun displayName(uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }

    private fun existing(name: String?): File? =
        name?.let { File(modelsDir, it).takeIf(File::exists) }

    private companion object {
        const val KEY_LAYOUT = "model.layout"
        const val KEY_VLM = "model.vlm"
        const val KEY_MMPROJ = "model.mmproj"
        const val KEY_MODE = "engine.mode"
        const val KEY_REMOTE = "engine.remote"
    }
}
