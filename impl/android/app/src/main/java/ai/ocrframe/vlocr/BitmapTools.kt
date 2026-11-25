package ai.ocrframe.vlocr

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import androidx.exifinterface.media.ExifInterface
import kotlin.math.max
import kotlin.math.roundToInt
import kotlin.math.sqrt

/** 超过 maxPixels 时等比缩小（共享核心还会再按块限制像素，这里只防整页内存爆掉）。 */
fun Bitmap.limitPixels(maxPixels: Int): Bitmap {
    val total = width.toLong() * height.toLong()
    if (total <= maxPixels) return this
    val factor = sqrt(maxPixels.toDouble() / total.toDouble())
    val targetWidth = max(1, (width * factor).roundToInt())
    val targetHeight = max(1, (height * factor).roundToInt())
    return Bitmap.createScaledBitmap(this, targetWidth, targetHeight, true)
}

/** 紧排 RGB（width*height*3），与共享核心的 RgbBuffer 约定一致。 */
fun Bitmap.toRgbBytes(): ByteArray {
    val pixels = IntArray(width * height)
    getPixels(pixels, 0, width, 0, 0, width, height)
    val out = ByteArray(width * height * 3)
    var index = 0
    for (pixel in pixels) {
        out[index++] = ((pixel shr 16) and 0xFF).toByte()
        out[index++] = ((pixel shr 8) and 0xFF).toByte()
        out[index++] = (pixel and 0xFF).toByte()
    }
    return out
}

fun Bitmap.thumbnail(maxDimension: Int): Bitmap {
    val longest = max(width, height)
    if (longest <= maxDimension) return this
    val ratio = maxDimension.toFloat() / longest
    return Bitmap.createScaledBitmap(
        this,
        max(1, (width * ratio).roundToInt()),
        max(1, (height * ratio).roundToInt()),
        true,
    )
}

/**
 * 解码 + EXIF 方向矫正。相机与相册的 URI 都走这里，避免出现「横着识别」。
 */
fun decodeBitmap(context: Context, uri: Uri, maxPixels: Int = VlEngine.MAX_PAGE_PIXELS): Bitmap? {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    context.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }

    var sample = 1
    if (bounds.outWidth > 0 && bounds.outHeight > 0) {
        val total = bounds.outWidth.toLong() * bounds.outHeight.toLong()
        while (total / (sample.toLong() * sample) > maxPixels * 2L) {
            sample *= 2
        }
    }

    val options = BitmapFactory.Options().apply { inSampleSize = sample }
    val decoded = context.contentResolver.openInputStream(uri)?.use {
        BitmapFactory.decodeStream(it, null, options)
    } ?: return null

    val rotation = context.contentResolver.openInputStream(uri)?.use { stream ->
        when (ExifInterface(stream).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)) {
            ExifInterface.ORIENTATION_ROTATE_90 -> 90f
            ExifInterface.ORIENTATION_ROTATE_180 -> 180f
            ExifInterface.ORIENTATION_ROTATE_270 -> 270f
            else -> 0f
        }
    } ?: 0f

    val rotated = if (rotation == 0f) {
        decoded
    } else {
        Bitmap.createBitmap(
            decoded, 0, 0, decoded.width, decoded.height,
            Matrix().apply { postRotate(rotation) }, true,
        ).also { if (it != decoded) decoded.recycle() }
    }
    return rotated.limitPixels(maxPixels)
}
