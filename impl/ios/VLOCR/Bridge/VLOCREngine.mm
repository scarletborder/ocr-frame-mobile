//
//  VLOCREngine.mm
//  iOS 平台胶水层。所有 VL 线规则都在 impl/shard 的 C++ 核心里，这里只有：
//    · CGImage → RGB（含大图降采样护栏）
//    · 远程识别回调的同步 HTTP 实现
//

#import "VLOCREngine.h"

#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "vlcore/vlcore_c.h"

/// 单页超过这个像素数先降采样：24MP 照片的 RGBA+RGB 双缓冲会吃掉 170MB。
static const long long kMaxPagePixels = 12'000'000;

static NSError *VLOCRError(NSString *message) {
    return [NSError errorWithDomain:@"VLOCREngine"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey: message ?: @"识别失败"}];
}

/// 接管 C 侧 malloc 的字符串并释放。
static NSString *VLOCRTakeString(char *text) {
    if (text == nullptr) return nil;
    NSString *value = [NSString stringWithUTF8String:text];
    vlcore_free_string(text);
    return value;
}

static void VLOCRSetError(NSError **error, char *message, NSString *fallback) {
    NSString *text = message != nullptr ? VLOCRTakeString(message) : fallback;
    if (error != nullptr) *error = VLOCRError(text);
}

static void VLOCRProgressTrampoline(int done, int total, void *user) {
    VLOCREngineProgress block = (__bridge VLOCREngineProgress)user;
    if (block != nil) block(done, total);
}

/// CGImage → 紧排 RGB（必要时等比缩小）。
static BOOL VLOCRRGBFromCGImage(CGImageRef image, std::vector<uint8_t> *out,
                                int *outWidth, int *outHeight) {
    if (image == NULL || out == nullptr) return NO;

    size_t width = CGImageGetWidth(image);
    size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) return NO;

    const long long total = static_cast<long long>(width) * static_cast<long long>(height);
    if (total > kMaxPagePixels) {
        const double factor = std::sqrt(static_cast<double>(kMaxPagePixels) / static_cast<double>(total));
        width = std::max<size_t>(1, static_cast<size_t>(std::llround(width * factor)));
        height = std::max<size_t>(1, static_cast<size_t>(std::llround(height * factor)));
    }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (context == NULL) return NO;

    CGContextSetRGBFillColor(context, 1.0, 1.0, 1.0, 1.0);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);

    const uint8_t *rgba = static_cast<const uint8_t *>(CGBitmapContextGetData(context));
    if (rgba == nullptr) {
        CGContextRelease(context);
        return NO;
    }

    out->resize(width * height * 3);
    for (size_t i = 0, count = width * height; i < count; i++) {
        (*out)[i * 3 + 0] = rgba[i * 4 + 0];
        (*out)[i * 3 + 1] = rgba[i * 4 + 1];
        (*out)[i * 3 + 2] = rgba[i * 4 + 2];
    }
    CGContextRelease(context);

    *outWidth = static_cast<int>(width);
    *outHeight = static_cast<int>(height);
    return YES;
}

/// 远程服务的连接参数（由引擎持有，指针生命周期与 C 引擎一致）。
@interface VLOCRRemoteConfig : NSObject
@property (nonatomic, copy) NSURL *baseURL;
@property (nonatomic, copy) NSString *modelName;
@end

@implementation VLOCRRemoteConfig
@end

static UIImage *VLOCRImageFromRGB(const uint8_t *rgb, int width, int height) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgb,
                                                              static_cast<size_t>(width) * height * 3, NULL);
    if (provider == NULL) {
        CGColorSpaceRelease(space);
        return nil;
    }
    CGImageRef image = CGImageCreate(width, height, 8, 24, static_cast<size_t>(width) * 3, space,
                                     kCGBitmapByteOrderDefault, provider, NULL, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    if (image == NULL) return nil;
    UIImage *result = [UIImage imageWithCGImage:image];
    CGImageRelease(image);
    return result;
}

/// 共享核心在需要逐块识别时回调到这里（同步阻塞，跑在后台线程）。
static char *VLOCRRemoteCallback(const uint8_t *rgb, int width, int height,
                                 const char *prompt, void *user, char **error) {
    VLOCRRemoteConfig *config = (__bridge VLOCRRemoteConfig *)user;
    @autoreleasepool {
        if (config == nil || config.baseURL == nil) {
            if (error != nullptr) *error = strdup("未配置远程识别服务地址。");
            return nullptr;
        }

        UIImage *image = VLOCRImageFromRGB(rgb, width, height);
        NSData *jpeg = image != nil ? UIImageJPEGRepresentation(image, 0.95) : nil;
        if (jpeg == nil) {
            if (error != nullptr) *error = strdup("图像编码失败。");
            return nullptr;
        }

        NSString *promptText = prompt != nullptr ? [NSString stringWithUTF8String:prompt] : @"OCR:";
        NSDictionary *imagePart = @{
            @"type": @"image_url",
            @"image_url": @{@"url": [@"data:image/jpeg;base64,"
                                     stringByAppendingString:[jpeg base64EncodedString]]},
        };
        NSDictionary *textPart = @{@"type": @"text", @"text": promptText ?: @"OCR:"};
        NSDictionary *message = @{@"role": @"user", @"content": @[imagePart, textPart]};
        NSDictionary *body = @{
            @"model": config.modelName ?: @"PaddleOCR-VL",
            @"temperature": @0,
            @"max_tokens": @(vlcore_max_new_tokens()),
            @"messages": @[message],
        };

        NSError *encodeError = nil;
        NSData *payload = [NSJSONSerialization dataWithJSONObject:body options:0 error:&encodeError];
        if (payload == nil) {
            if (error != nullptr) *error = strdup(encodeError.localizedDescription.UTF8String ?: "请求体编码失败。");
            return nullptr;
        }

        NSURL *endpoint = [config.baseURL URLByAppendingPathComponent:@"chat/completions"];
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:endpoint];
        request.HTTPMethod = @"POST";
        request.timeoutInterval = 300;
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        request.HTTPBody = payload;

        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        __block NSData *responseData = nil;
        __block NSError *responseError = nil;
        [[[NSURLSession sharedSession] dataTaskWithRequest:request
                                        completionHandler:^(NSData *data, NSURLResponse *response, NSError *taskError) {
            responseData = data;
            responseError = taskError;
            dispatch_semaphore_signal(semaphore);
        }] resume];
        dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(300 * NSEC_PER_SEC)));

        if (responseError != nil || responseData == nil) {
            if (error != nullptr) {
                *error = strdup(responseError.localizedDescription.UTF8String ?: "远程服务无响应。");
            }
            return nullptr;
        }

        id root = [NSJSONSerialization JSONObjectWithData:responseData options:0 error:nil];
        id choices = [root isKindOfClass:[NSDictionary class]] ? [root objectForKey:@"choices"] : nil;
        id first = [choices isKindOfClass:[NSArray class]] && [choices count] > 0 ? [choices objectAtIndex:0] : nil;
        id messageDict = [first isKindOfClass:[NSDictionary class]] ? [first objectForKey:@"message"] : nil;
        id content = [messageDict isKindOfClass:[NSDictionary class]] ? [messageDict objectForKey:@"content"] : nil;
        if (![content isKindOfClass:[NSString class]]) {
            if (error != nullptr) *error = strdup("远程服务返回结构不符合 OpenAI 兼容格式。");
            return nullptr;
        }
        return strdup(content.UTF8String);
    }
}

@implementation VLOCREngine {
    vlcore_engine_t *_engine;
    VLOCRRemoteConfig *_remote;
}

+ (BOOL)layoutEngineAvailable {
    return vlcore_layout_engine_built() != 0;
}

+ (BOOL)localVlmAvailable {
    return vlcore_vlm_engine_built() != 0;
}

+ (NSString *)presetName {
    return [NSString stringWithUTF8String:vlcore_preset_name()];
}

+ (NSString *)presetVersion {
    return [NSString stringWithUTF8String:vlcore_preset_version()];
}

+ (NSString *)pipelineVersion {
    return [NSString stringWithUTF8String:vlcore_pipeline_version()];
}

+ (NSInteger)maxNewTokens {
    return vlcore_max_new_tokens();
}

- (instancetype)initWithLayoutPath:(NSString *)layoutPath
                           vlmPath:(NSString *)vlmPath
                        mmprojPath:(NSString *)mmprojPath
                              error:(NSError **)error {
    self = [super init];
    if (self == nil) return nil;

    char *message = nullptr;
    _engine = vlcore_create(layoutPath.length > 0 ? layoutPath.fileSystemRepresentation : nullptr,
                            vlmPath.length > 0 ? vlmPath.fileSystemRepresentation : nullptr,
                            mmprojPath.length > 0 ? mmprojPath.fileSystemRepresentation : nullptr,
                            8192, 4, &message);
    if (_engine == nullptr) {
        VLOCRSetError(error, message, @"识别引擎初始化失败。");
        return nil;
    }
    return self;
}

- (BOOL)useRemoteServiceWithBaseURL:(NSURL *)baseURL
                              model:(NSString *)modelName
                              error:(NSError **)error {
    if (_engine == nullptr) {
        if (error != nullptr) *error = VLOCRError(@"识别引擎未初始化。");
        return NO;
    }
    VLOCRRemoteConfig *config = [[VLOCRRemoteConfig alloc] init];
    config.baseURL = baseURL;
    config.modelName = modelName.length > 0 ? modelName : @"PaddleOCR-VL";
    _remote = config;
    vlcore_set_remote(_engine, VLOCRRemoteCallback, (__bridge void *)config);
    return YES;
}

- (NSString *)runJSONOnImage:(CGImageRef)image
                    progress:(VLOCREngineProgress)progress
                       error:(NSError **)error {
    if (_engine == nullptr) {
        if (error != nullptr) *error = VLOCRError(@"识别引擎未初始化。");
        return nil;
    }

    std::vector<uint8_t> rgb;
    int width = 0;
    int height = 0;
    if (!VLOCRRGBFromCGImage(image, &rgb, &width, &height)) {
        if (error != nullptr) *error = VLOCRError(@"图片无法解码，请重新拍摄或选择。");
        return nil;
    }

    char *message = nullptr;
    char *json = vlcore_run(_engine, rgb.data(), width, height,
                            VLOCRProgressTrampoline, (__bridge void *)progress, &message);
    if (json == nullptr) {
        VLOCRSetError(error, message, @"识别失败。");
        return nil;
    }
    return VLOCRTakeString(json);
}

- (void)dealloc {
    if (_engine != nullptr) {
        vlcore_destroy(_engine);
        _engine = nullptr;
    }
}

@end
