/* vlcore C 门面：iOS（ObjC++）与 Android（JNI）都只调这一组函数。
 *
 * 约定：
 *   · 返回的 char* 一律由调用方用 vlcore_free_string 释放；
 *   · error 出参为 malloc 的 UTF-8 文本（可为 NULL），调用方同样用 vlcore_free_string 释放；
 *   · 输入图像是紧排 RGB（width*height*3），平台侧负责解码（CGImage / Bitmap）。
 */
#ifndef VLCORE_C_H
#define VLCORE_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vlcore_engine vlcore_engine_t;

typedef void (*vlcore_progress_fn)(int done, int total, void *user);

/* 远程识别回调：返回 malloc 的文本（核心负责 free）；失败返回 NULL 并填 *error。 */
typedef char *(*vlcore_remote_fn)(const uint8_t *rgb, int width, int height,
                                  const char *prompt, void *user, char **error);

/* 任一模型路径传 NULL / 空串表示不加载该引擎（例如只做版面）。 */
vlcore_engine_t *vlcore_create(const char *layout_onnx_path,
                               const char *vlm_gguf_path,
                               const char *mmproj_gguf_path,
                               int n_ctx, int n_threads, char **error);

void vlcore_destroy(vlcore_engine_t *engine);

/* 注册远程识别服务（未加载本地 VLM 时使用）。 */
void vlcore_set_remote(vlcore_engine_t *engine, vlcore_remote_fn fn, void *user);

int vlcore_has_layout(const vlcore_engine_t *engine);
int vlcore_has_vlm(const vlcore_engine_t *engine);

/* 整页：版面 + 逐块识别 + Markdown。
 * 返回 {"markdown":"…","blocks":[{"label","order","score","bbox","text","recognized"}],"seconds":1.23} */
char *vlcore_run(vlcore_engine_t *engine, const uint8_t *rgb, int width, int height,
                 vlcore_progress_fn progress, void *user, char **error);

/* 只做版面分析，返回块 JSON 数组。 */
char *vlcore_layout_json(vlcore_engine_t *engine, const uint8_t *rgb, int width, int height,
                         char **error);

/* 单块识别（label 决定提示词，如 text / table / display_formula）。 */
char *vlcore_recognize_block(vlcore_engine_t *engine, const uint8_t *rgb, int width, int height,
                             const char *label, char **error);

/* 官方提示词（"OCR:" 等），平台侧不需要自己维护标签表。 */
char *vlcore_prompt_for_label(const char *label);

/* 该引擎是否编译进本次构建（区分「没选模型」与「没链接库」）。 */
int vlcore_layout_engine_built(void);
int vlcore_vlm_engine_built(void);

const char *vlcore_preset_name(void);
const char *vlcore_preset_version(void);
const char *vlcore_pipeline_version(void);
int vlcore_max_new_tokens(void);

void vlcore_free_string(char *text);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* VLCORE_C_H */
