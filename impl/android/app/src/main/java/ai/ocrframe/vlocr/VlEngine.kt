package ai.ocrframe.vlocr

import android.graphics.Bitmap

/** 远程识别回调：在 nativeRunJSON 的调用线程上同步执行，返回识别文本。 */
fun interface RemoteRecognizer {
    fun recognize(rgb: ByteArray, width: Int, height: Int, prompt: String): String
}

fun interface ProgressListener {
    fun onProgress(done: Int, total: Int)
}

/**
 * 共享核心（impl/shard）的 Kotlin 门面。所有 VL 线规则都在 C++ 侧：
 * 版面后处理、阅读顺序、提示词、Markdown 合并、两阶段编排。
 */
class VlEngine private constructor(private val handle: Long) : AutoCloseable {

    /** 整页识别，返回核心 JSON：{"markdown":…,"blocks":[…],"seconds":…}。阻塞调用。 */
    fun runJSON(bitmap: Bitmap, listener: ProgressListener? = null): String {
        val limited = bitmap.limitPixels(MAX_PAGE_PIXELS)
        val rgb = limited.toRgbBytes()
        return nativeRunJSON(handle, rgb, limited.width, limited.height, listener)
    }

    fun useRemote(recognizer: RemoteRecognizer) {
        nativeSetRemote(handle, recognizer)
    }

    override fun close() {
        nativeDestroy(handle)
    }

    companion object {
        /** 24MP 照片的 RGB 缓冲会吃掉 70MB+，进核心前先降采样。 */
        const val MAX_PAGE_PIXELS = 12_000_000

        init {
            System.loadLibrary("vlocr")
        }

        fun create(layoutPath: String, vlmPath: String?, mmprojPath: String?): VlEngine {
            val pointer = nativeCreate(layoutPath, vlmPath, mmprojPath, 8192, 4)
            check(pointer != 0L) { "识别引擎初始化失败" }
            return VlEngine(pointer)
        }

        fun layoutEngineBuilt(): Boolean = nativeLayoutEngineBuilt()

        fun vlmEngineBuilt(): Boolean = nativeVlmEngineBuilt()

        fun presetName(): String = nativePresetName()

        fun presetVersion(): String = nativePresetVersion()

        fun pipelineVersion(): String = nativePipelineVersion()

        fun maxNewTokens(): Int = nativeMaxNewTokens()

        @JvmStatic
        private external fun nativeCreate(
            layoutPath: String,
            vlmPath: String?,
            mmprojPath: String?,
            contextSize: Int,
            threads: Int,
        ): Long

        @JvmStatic
        private external fun nativeDestroy(handle: Long)

        @JvmStatic
        private external fun nativeSetRemote(handle: Long, recognizer: RemoteRecognizer?)

        @JvmStatic
        private external fun nativeRunJSON(
            handle: Long,
            rgb: ByteArray,
            width: Int,
            height: Int,
            listener: ProgressListener?,
        ): String

        @JvmStatic
        private external fun nativeLayoutEngineBuilt(): Boolean

        @JvmStatic
        private external fun nativeVlmEngineBuilt(): Boolean

        @JvmStatic
        private external fun nativePresetName(): String

        @JvmStatic
        private external fun nativePresetVersion(): String

        @JvmStatic
        private external fun nativePipelineVersion(): String

        @JvmStatic
        private external fun nativeMaxNewTokens(): Int
    }
}
