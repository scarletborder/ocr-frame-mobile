package ai.ocrframe.vlocr

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * 引擎只创建一次并缓存（模型常驻），签名变化时才重建。
 */
class EngineHost {

    private var engine: VlEngine? = null
    private var signature: String = ""

    suspend fun pipeline(store: ModelStore): VlEngine = withContext(Dispatchers.IO) {
        val current = signature(store)
        engine?.let { if (current == signature) return@withContext it }

        val layout = store.layoutFile ?: error("请先在「设置」里选择版面分析模型。")
        if (!VlEngine.layoutEngineBuilt()) {
            error("当前构建未包含 ONNX Runtime 版面引擎。请先运行 scripts/fetch-onnxruntime-android.sh 后重新构建。")
        }

        val local = store.mode == ModelStore.Mode.LOCAL
        if (local) {
            val vlm = store.vlmFile ?: error("请先在「设置」里选择视觉语言识别模型。")
            val mmproj = store.mmprojFile ?: error("缺少视觉投影器（*-mmproj.gguf）。")
            if (!VlEngine.vlmEngineBuilt()) {
                error("当前构建未包含 llama.cpp 视觉引擎。请先运行 scripts/build-llama-android.sh，或改用远程服务模式。")
            }
            engine?.close()
            val created = VlEngine.create(layout.absolutePath, vlm.absolutePath, mmproj.absolutePath)
            engine = created
            signature = current
            return@withContext created
        }

        engine?.close()
        val created = VlEngine.create(layout.absolutePath, null, null)
        val baseUrl = store.remoteBaseUrl
        created.useRemote { rgb, width, height, prompt ->
            RemoteClient.recognize(baseUrl, rgb, width, height, prompt)
        }
        engine = created
        signature = current
        created
    }

    fun invalidate() {
        engine?.close()
        engine = null
        signature = ""
    }

    private fun signature(store: ModelStore): String = listOf(
        store.layoutFile?.absolutePath ?: "-",
        store.vlmFile?.absolutePath ?: "-",
        store.mmprojFile?.absolutePath ?: "-",
        store.mode.name,
        store.remoteBaseUrl,
    ).joinToString("|")
}
