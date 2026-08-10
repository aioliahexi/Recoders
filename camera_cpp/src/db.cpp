#include "db.h"

#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace camera {

Db::Db(const std::string& path) {
    // 确保父目录存在（否则 sqlite3_open 静默失败）
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        db_ = nullptr;
        return;
    }
    auto exec = [&](const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK && err) {
            fprintf(stderr, "[db] %s: %s\n", sql, err);
            sqlite3_free(err);
        }
        return rc;
    };
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("CREATE TABLE IF NOT EXISTS recordings ("
         " id INTEGER PRIMARY KEY AUTOINCREMENT,"
         " camera_id INTEGER NOT NULL,"
         " camera_name TEXT,"
         " start_ms INTEGER NOT NULL,"
         " end_ms INTEGER,"
         " size_bytes INTEGER DEFAULT 0,"
         " path TEXT NOT NULL UNIQUE,"
         " camera_uuid TEXT);");
    exec("CREATE TABLE IF NOT EXISTS oplog ("
         " id INTEGER PRIMARY KEY AUTOINCREMENT,"
         " ts_ms INTEGER NOT NULL,"
         " camera_id INTEGER DEFAULT 0,"
         " action TEXT,"
         " detail TEXT,"
         " result INTEGER DEFAULT 1);");
    exec("CREATE TABLE IF NOT EXISTS photos ("
         " id INTEGER PRIMARY KEY AUTOINCREMENT,"
         " camera_id INTEGER NOT NULL,"
         " name TEXT NOT NULL,"
         " ts_ms INTEGER NOT NULL,"
         " tag TEXT DEFAULT '手动抓拍',"
         " source TEXT DEFAULT 'manual',"
         " UNIQUE(camera_id,name));");
    // 存量库迁移：补 camera_uuid 列
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT camera_uuid FROM recordings LIMIT 1;", -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_finalize(st);
            exec("ALTER TABLE recordings ADD COLUMN camera_uuid TEXT;");
        } else {
            sqlite3_finalize(st);
        }
    }
}

Db::~Db() {
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    sqlite3_close(db_);
}

void Db::upsert(const DbSegment& s) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO recordings(camera_id,camera_name,start_ms,end_ms,size_bytes,path,camera_uuid)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET start_ms=excluded.start_ms,"
        " end_ms=excluded.end_ms, size_bytes=excluded.size_bytes, camera_uuid=excluded.camera_uuid;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, s.camera_id);
    sqlite3_bind_text(st, 2, s.camera_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(s.start_ms));
    sqlite3_bind_int64(st, 4, static_cast<sqlite3_int64>(s.end_ms));
    sqlite3_bind_int64(st, 5, static_cast<sqlite3_int64>(s.size_bytes));
    sqlite3_bind_text(st, 6, s.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, s.camera_uuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::optional<DbSegment> Db::get_by_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT * FROM recordings WHERE path=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<DbSegment> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        DbSegment s;
        s.id = sqlite3_column_int64(st, 0);
        s.camera_id = sqlite3_column_int(st, 1);
        s.camera_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        s.start_ms = sqlite3_column_int64(st, 3);
        s.end_ms = sqlite3_column_int64(st, 4);
        s.size_bytes = sqlite3_column_int64(st, 5);
        s.path = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
        {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
            if (u) s.camera_uuid = u;   // 存量库该列为 NULL，必须判空
        }
        out = s;
    }
    sqlite3_finalize(st);
    return out;
}

std::optional<DbSegment> Db::get_by_id(int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT * FROM recordings WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    std::optional<DbSegment> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        DbSegment s;
        s.id = sqlite3_column_int64(st, 0);
        s.camera_id = sqlite3_column_int(st, 1);
        s.camera_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        s.start_ms = sqlite3_column_int64(st, 3);
        s.end_ms = sqlite3_column_int64(st, 4);
        s.size_bytes = sqlite3_column_int64(st, 5);
        s.path = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
        {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
            if (u) s.camera_uuid = u;   // 存量库该列为 NULL，必须判空
        }
        out = s;
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<DbSegment> Db::list(int camera_id, uint64_t start_ms, uint64_t end_ms,
                                int page, int page_size, int* total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where;
    if (camera_id > 0) where += " WHERE camera_id=?";
    if (start_ms) where += (where.empty() ? " WHERE " : " AND ") + std::string("start_ms>=?");
    if (end_ms) where += (where.empty() ? " WHERE " : " AND ") + std::string("start_ms<=?");
    std::string sql = "SELECT COUNT(*) FROM recordings" + where + ";";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr);
    int idx = 1;
    if (camera_id > 0) sqlite3_bind_int(st, idx++, camera_id);
    if (start_ms) sqlite3_bind_int64(st, idx++, start_ms);
    if (end_ms) sqlite3_bind_int64(st, idx++, end_ms);
    if (total) {
        *total = 0;
        if (sqlite3_step(st) == SQLITE_ROW) *total = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);

    sql = "SELECT * FROM recordings" + where +
          " ORDER BY start_ms DESC LIMIT ? OFFSET ?;";
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr);
    idx = 1;
    if (camera_id > 0) sqlite3_bind_int(st, idx++, camera_id);
    if (start_ms) sqlite3_bind_int64(st, idx++, start_ms);
    if (end_ms) sqlite3_bind_int64(st, idx++, end_ms);
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);

    std::vector<DbSegment> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        DbSegment s;
        s.id = sqlite3_column_int64(st, 0);
        s.camera_id = sqlite3_column_int(st, 1);
        s.camera_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        s.start_ms = sqlite3_column_int64(st, 3);
        s.end_ms = sqlite3_column_int64(st, 4);
        s.size_bytes = sqlite3_column_int64(st, 5);
        s.path = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
        {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
            if (u) s.camera_uuid = u;   // 存量库该列为 NULL，必须判空
        }
        out.push_back(s);
    }
    sqlite3_finalize(st);
    return out;
}

void Db::add_op(uint64_t ts_ms, int camera_id, const std::string& action,
                const std::string& detail, int result) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT INTO oplog(ts_ms,camera_id,action,detail,result) VALUES(?,?,?,?,?);",
        -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(ts_ms));
    sqlite3_bind_int(st, 2, camera_id);
    sqlite3_bind_text(st, 3, action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, result);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::vector<OpLogEntry> Db::list_ops(int page, int page_size, int* total) {
    std::lock_guard<std::mutex> lk(mu_);
    if (page < 1) page = 1;
    if (page_size < 1 || page_size > 500) page_size = 50;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM oplog;", -1, &st, nullptr);
    if (total) {
        *total = 0;
        if (sqlite3_step(st) == SQLITE_ROW) *total = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db_,
        "SELECT id,ts_ms,camera_id,action,detail,result FROM oplog"
        " ORDER BY id DESC LIMIT ? OFFSET ?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, page_size);
    sqlite3_bind_int(st, 2, (page - 1) * page_size);
    std::vector<OpLogEntry> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        OpLogEntry e;
        e.id = sqlite3_column_int64(st, 0);
        e.ts_ms = sqlite3_column_int64(st, 1);
        e.camera_id = sqlite3_column_int(st, 2);
        e.action = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
        e.detail = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        e.result = sqlite3_column_int(st, 5);
        out.push_back(e);
    }
    sqlite3_finalize(st);
    return out;
}

void Db::add_photo(int camera_id, const std::string& name, uint64_t ts_ms,
                   const std::string& tag, const std::string& source) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO photos(camera_id,name,ts_ms,tag,source) VALUES(?,?,?,?,?);",
        -1, &st, nullptr);
    sqlite3_bind_int(st, 1, camera_id);
    sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(ts_ms));
    sqlite3_bind_text(st, 4, tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

bool Db::delete_photo_by_name(int camera_id, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM photos WHERE camera_id=? AND name=?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, camera_id);
    sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::vector<PhotoRecord> Db::list_photos(int camera_id, const std::string& tag,
                                         int page, int page_size, int* total) {
    std::lock_guard<std::mutex> lk(mu_);
    if (page < 1) page = 1;
    if (page_size < 1 || page_size > 500) page_size = 24;
    std::string where = " WHERE camera_id=?";
    if (!tag.empty()) where += " AND tag=?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, ("SELECT COUNT(*) FROM photos" + where + ";").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, camera_id);
    if (!tag.empty()) sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
    if (total) {
        *total = 0;
        if (sqlite3_step(st) == SQLITE_ROW) *total = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db_, ("SELECT id,camera_id,name,ts_ms,tag,source FROM photos" + where +
                             " ORDER BY ts_ms DESC LIMIT ? OFFSET ?;").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, camera_id);
    int idx = 2;
    if (!tag.empty()) sqlite3_bind_text(st, idx++, tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);
    std::vector<PhotoRecord> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        PhotoRecord r;
        r.id = sqlite3_column_int64(st, 0);
        r.camera_id = sqlite3_column_int(st, 1);
        r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        r.ts_ms = sqlite3_column_int64(st, 3);
        r.tag = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
        r.source = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
        out.push_back(r);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<std::string> Db::list_photo_tags(int camera_id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT DISTINCT tag FROM photos WHERE camera_id=? ORDER BY tag;",
                       -1, &st, nullptr);
    sqlite3_bind_int(st, 1, camera_id);
    std::vector<std::string> out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    sqlite3_finalize(st);
    return out;
}

int Db::update_path_prefix(const std::string& old_prefix, const std::string& new_prefix) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "UPDATE recordings SET path = ? || substr(path, ?) WHERE path LIKE ?;",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, new_prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, static_cast<int>(old_prefix.size()) + 1);
    sqlite3_bind_text(st, 3, (old_prefix + "%").c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    int changes = (rc == SQLITE_DONE) ? sqlite3_changes(db_) : 0;
    sqlite3_finalize(st);
    return changes;
}

std::vector<DbSegment> Db::oldest(int limit) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_,
        "SELECT * FROM recordings ORDER BY start_ms ASC LIMIT ?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, limit);
    std::vector<DbSegment> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        DbSegment s;
        s.id = sqlite3_column_int64(st, 0);
        s.camera_id = sqlite3_column_int(st, 1);
        s.camera_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
        s.start_ms = sqlite3_column_int64(st, 3);
        s.end_ms = sqlite3_column_int64(st, 4);
        s.size_bytes = sqlite3_column_int64(st, 5);
        s.path = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
        {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
            if (u) s.camera_uuid = u;   // 存量库该列为 NULL，必须判空
        }
        out.push_back(s);
    }
    sqlite3_finalize(st);
    return out;
}

bool Db::remove_by_id(int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM recordings WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

uint64_t Db::total_size() {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COALESCE(SUM(size_bytes),0) FROM recordings;", -1, &st, nullptr);
    uint64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

int Db::count() {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM recordings;", -1, &st, nullptr);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

}  // namespace camera
