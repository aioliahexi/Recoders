// SQLite 录像索引（WAL，线程安全）
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace camera {

struct OpLogEntry {
    int64_t id = 0;
    uint64_t ts_ms = 0;
    int camera_id = 0;
    std::string action;
    std::string detail;
    int result = 0;   // 1 成功 / 0 失败
};

struct DbSegment {
    int64_t id = 0;
    int camera_id = 0;
    std::string camera_name;
    std::string camera_uuid;   // 物理相机设备 UUID（绑定设备用）
    uint64_t start_ms = 0, end_ms = 0;
    uint64_t size_bytes = 0;
    std::string path;
};

// 照片/事件索引（事件标签存数据库；文件名保持纯净 <ts>.jpg）
struct PhotoRecord {
    int64_t id = 0;
    int camera_id = 0;
    std::string name;      // 文件名（含 .jpg）
    uint64_t ts_ms = 0;
    std::string tag;       // 事件标签，如 入侵报警/手动抓拍
    std::string source;    // manual=手动抓拍 / event=事件触发
};

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void upsert(const DbSegment& seg);
    std::optional<DbSegment> get_by_path(const std::string& path);
    std::optional<DbSegment> get_by_id(int64_t id);
    std::vector<DbSegment> list(int camera_id, uint64_t start_ms, uint64_t end_ms,
                                int page, int page_size, int* total);
    std::vector<DbSegment> oldest(int limit);
    bool remove_by_id(int64_t id);
    // 录像目录改名后同步更新 DB 中的绝对路径前缀（返回更新的行数）
    int update_path_prefix(const std::string& old_prefix, const std::string& new_prefix);

    // 操作日志（系统页动作审计）
    void add_op(uint64_t ts_ms, int camera_id, const std::string& action,
                const std::string& detail, int result);
    std::vector<OpLogEntry> list_ops(int page, int page_size, int* total);

    // 照片/事件索引
    void add_photo(int camera_id, const std::string& name, uint64_t ts_ms,
                   const std::string& tag, const std::string& source);
    bool delete_photo_by_name(int camera_id, const std::string& name);
    std::vector<PhotoRecord> list_photos(int camera_id, const std::string& tag,
                                         int page, int page_size, int* total);
    std::vector<std::string> list_photo_tags(int camera_id);
    uint64_t total_size();
    int count();

private:
    sqlite3* db_ = nullptr;
    std::mutex mu_;
};

}  // namespace camera
