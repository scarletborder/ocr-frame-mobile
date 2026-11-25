package ai.ocrframe.vlocr

import android.graphics.Bitmap
import org.json.JSONArray
import org.json.JSONObject

data class BlockResult(
    val label: String,
    val order: Int,
    val text: String,
    val recognized: Boolean,
)

data class PageResult(
    val image: Bitmap,
    val markdown: String,
    val blocks: List<BlockResult>,
    val seconds: Double,
)

/** 共享核心返回的 JSON（见 impl/shard/src/vlcore_c.cpp::RunResultJson）。 */
object VlResultParser {

    fun parse(json: String, image: Bitmap): PageResult {
        val root = JSONObject(json)
        val markdown = root.optString("markdown")
        val seconds = root.optDouble("seconds", 0.0)
        val array = root.optJSONArray("blocks") ?: JSONArray()
        val blocks = ArrayList<BlockResult>(array.length())
        for (index in 0 until array.length()) {
            val item = array.optJSONObject(index) ?: continue
            blocks += BlockResult(
                label = item.optString("label"),
                order = item.optInt("order", -1),
                text = item.optString("text"),
                recognized = item.optBoolean("recognized", false),
            )
        }
        return PageResult(image = image, markdown = markdown, blocks = blocks, seconds = seconds)
    }
}
