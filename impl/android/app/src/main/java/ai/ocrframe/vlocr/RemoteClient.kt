package ai.ocrframe.vlocr

import android.graphics.Bitmap
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * 远程识别（llama.cpp server / mlx-vlm server 的 OpenAI 兼容接口）。
 *
 * 只在「远程服务」模式下被 EngineHost 注册为共享核心的回调；
 * 调用发生在 nativeRunJSON 的同一线程（后台线程），因此这里用同步 HttpURLConnection。
 */
object RemoteClient {

    fun recognize(baseUrl: String, rgb: ByteArray, width: Int, height: Int, prompt: String): String {
        val pixels = IntArray(width * height)
        for (index in pixels.indices) {
            val r = rgb[index * 3].toInt() and 0xFF
            val g = rgb[index * 3 + 1].toInt() and 0xFF
            val b = rgb[index * 3 + 2].toInt() and 0xFF
            pixels[index] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        bitmap.setPixels(pixels, 0, width, 0, 0, width, height)

        val jpeg = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.JPEG, 95, jpeg)
        bitmap.recycle()

        val dataUrl = "data:image/jpeg;base64," +
            Base64.encodeToString(jpeg.toByteArray(), Base64.NO_WRAP)

        val body = JSONObject().apply {
            put("model", "PaddleOCR-VL")
            put("temperature", 0)
            put("max_tokens", VlEngine.maxNewTokens())
            put(
                "messages",
                JSONArray().put(
                    JSONObject().apply {
                        put("role", "user")
                        put(
                            "content",
                            JSONArray().apply {
                                put(
                                    JSONObject().apply {
                                        put("type", "image_url")
                                        put("image_url", JSONObject().put("url", dataUrl))
                                    },
                                )
                                put(JSONObject().apply { put("type", "text"); put("text", prompt) })
                            },
                        )
                    },
                ),
            )
        }.toString()

        val connection = (URL(baseUrl.trimEnd('/') + "/chat/completions").openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = 15_000
            readTimeout = 300_000
            doOutput = true
            setRequestProperty("Content-Type", "application/json")
        }

        try {
            connection.outputStream.use { it.write(body.toByteArray()) }
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() }.orEmpty()
            if (code !in 200..299) {
                throw IllegalStateException("远程服务返回 $code：${text.take(200)}")
            }
            val content = JSONObject(text)
                .optJSONArray("choices")
                ?.optJSONObject(0)
                ?.optJSONObject("message")
                ?.optString("content")
            if (content.isNullOrEmpty()) {
                throw IllegalStateException("远程服务返回结构不符合 OpenAI 兼容格式。")
            }
            return content
        } finally {
            connection.disconnect()
        }
    }
}
