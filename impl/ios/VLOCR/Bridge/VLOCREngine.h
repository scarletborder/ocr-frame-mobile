//
//  VLOCREngine.h
//  iOS 侧唯一的推理门面：内部调用 impl/shard 的 C 接口（vlcore_c.h）。
//
//  版面分析、提示词、阅读顺序、Markdown 合并全部在共享核心里实现，
//  这里只负责两件平台相关的事：
//    1. CGImage → 紧排 RGB 字节（核心只认 RGB 缓冲）；
//    2. 远程识别回调（OpenAI 兼容 /v1/chat/completions）的 HTTP 实现。
//

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^VLOCREngineProgress)(NSInteger done, NSInteger total);

@interface VLOCREngine : NSObject

/// 版面引擎（ONNX Runtime）是否被编译进本次构建。
+ (BOOL)layoutEngineAvailable NS_SWIFT_NAME(layoutEngineAvailable());
/// 本地 VLM（llama.cpp + libmtmd）是否被编译进本次构建。
+ (BOOL)localVlmAvailable NS_SWIFT_NAME(localVlmAvailable());

/// 预设信息由共享核心提供，避免两端各写一份。
+ (NSString *)presetName NS_SWIFT_NAME(presetName());
+ (NSString *)presetVersion NS_SWIFT_NAME(presetVersion());
+ (NSString *)pipelineVersion NS_SWIFT_NAME(pipelineVersion());
+ (NSInteger)maxNewTokens NS_SWIFT_NAME(maxNewTokens());

/// layoutPath 必填；vlmPath / mmprojPath 传 nil 表示只做版面（配合远程服务）。
- (nullable instancetype)initWithLayoutPath:(NSString *)layoutPath
                                    vlmPath:(nullable NSString *)vlmPath
                                 mmprojPath:(nullable NSString *)mmprojPath
                                       error:(NSError **)error
    NS_SWIFT_NAME(init(layoutPath:vlmPath:mmprojPath:));

/// 注册远程识别服务（baseURL 需包含 /v1）。
- (BOOL)useRemoteServiceWithBaseURL:(NSURL *)baseURL
                              model:(NSString *)modelName
                              error:(NSError **)error
    NS_SWIFT_NAME(useRemoteService(baseURL:model:));

/// 整页识别，返回共享核心的 JSON：{"markdown":…,"blocks":[…],"seconds":…}。
- (nullable NSString *)runJSONOnImage:(CGImageRef)image
                             progress:(nullable VLOCREngineProgress)progress
                                error:(NSError **)error
    NS_SWIFT_NAME(runJSON(_:progress:));

@end

NS_ASSUME_NONNULL_END
