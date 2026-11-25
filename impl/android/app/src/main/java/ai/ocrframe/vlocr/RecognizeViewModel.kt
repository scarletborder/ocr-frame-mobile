package ai.ocrframe.vlocr

import android.graphics.Bitmap
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** 一次选图 → 逐页识别 → 展示结果；不做保存与项目。 */
class RecognizeViewModel : ViewModel() {

    sealed interface Status {
        data object Pending : Status
        data class Running(val done: Int, val total: Int) : Status
        data class Done(val result: PageResult) : Status
        data class Failed(val message: String) : Status
    }

    data class Item(
        val id: Long,
        val bitmap: Bitmap,
        val thumbnail: Bitmap,
        val status: Status,
    )

    private val _items = MutableStateFlow<List<Item>>(emptyList())
    val items: StateFlow<List<Item>> = _items.asStateFlow()

    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    fun reportError(message: String) {
        _error.value = message
    }

    fun clear() {
        if (_running.value) return
        _items.value = emptyList()
        _error.value = null
    }

    fun recognize(images: List<Bitmap>, store: ModelStore, host: EngineHost) {
        if (images.isEmpty() || _running.value) return
        _error.value = null
        _items.value = images.mapIndexed { index, bitmap ->
            Item(index.toLong(), bitmap, bitmap.thumbnail(320), Status.Pending)
        }
        _running.value = true

        viewModelScope.launch {
            try {
                val engine = host.pipeline(store)
                repeat(images.size) { index ->
                    setStatus(index, Status.Running(0, 0))
                    val bitmap = images[index]
                    try {
                        val json = withContext(Dispatchers.Default) {
                            engine.runJSON(bitmap) { done, total ->
                                setStatus(index, Status.Running(done, total))
                            }
                        }
                        setStatus(index, Status.Done(VlResultParser.parse(json, bitmap)))
                    } catch (error: Exception) {
                        setStatus(index, Status.Failed(error.message ?: "识别失败"))
                    }
                }
            } catch (error: Exception) {
                _error.value = error.message ?: "识别失败"
                _items.update { list ->
                    list.map { item ->
                        if (item.status is Status.Pending) item.copy(status = Status.Failed("未执行")) else item
                    }
                }
            } finally {
                _running.value = false
            }
        }
    }

    private fun setStatus(index: Int, status: Status) {
        _items.update { list ->
            if (index !in list.indices) return@update list
            list.toMutableList().also { it[index] = it[index].copy(status = status) }
        }
    }
}
