#include "server/storage/store.hpp"

#include <memory>

namespace nvr::web
{
namespace
{
Json decode_rows(Json rows, const std::string &column)
{
    for (auto &row : rows)
    {
        row[column == "config_json" ? "config"
            : column == "info_json" ? "info"
                                    : "detail"] = Json::parse(row.at(column).get<std::string>());
        row.erase(column);
    }
    return rows;
}
} // namespace

// Database connection and query helpers.
Store::Store(const fs::path &database)
{
    fs::create_directories(database.parent_path());
    check(sqlite3_open_v2(database.c_str(), &db_,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                          nullptr) == SQLITE_OK,
          "open application database", 500);
    sqlite3_busy_timeout(db_, 5000);
    execute("PRAGMA foreign_keys=ON");
    execute("PRAGMA journal_mode=WAL");
    execute(
        "CREATE TABLE IF NOT EXISTS assets(id TEXT PRIMARY KEY,kind TEXT NOT NULL CHECK(kind IN "
        "('video','model')),name TEXT NOT NULL,status TEXT NOT NULL,path TEXT NOT NULL "
        "UNIQUE,info_json TEXT NOT NULL,created_at TEXT NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS channels(id TEXT PRIMARY KEY,name TEXT NOT NULL,video_id "
            "TEXT REFERENCES assets(id) ON DELETE RESTRICT,model_id TEXT REFERENCES assets(id) ON "
            "DELETE RESTRICT,config_json TEXT NOT NULL,created_at TEXT NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY,kind TEXT NOT NULL,state TEXT NOT "
            "NULL,detail_json TEXT NOT NULL,created_at TEXT NOT NULL)");
    execute("PRAGMA user_version=1");
    // A process restart cannot truthfully resume an in-memory validation task.
    auto interrupted = query("SELECT id,detail_json FROM jobs WHERE state IN ('downloading','validating')");
    for (auto &row : interrupted)
    {
        auto job = Json::parse(row["detail_json"].get<std::string>());
        job["state"] = "rejected";
        job["error"] = "Import interrupted by server restart. Submit it again.";
        save_job(job);
    }
}

Store::~Store()
{
    if (db_)
    {
        sqlite3_close(db_);
    }
}

Json Store::query(const std::string &sql, const std::vector<Json> &values) const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sqlite3_stmt *raw = nullptr;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) == SQLITE_OK,
          "database statement failed", 500);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    for (size_t i = 0; i < values.size(); ++i)
    {
        const auto &value = values[i];
        int result;
        if (value.is_null())
        {
            result = sqlite3_bind_null(raw, int(i + 1));
        }
        else if (value.is_number_integer())
        {
            result = sqlite3_bind_int64(raw, int(i + 1), value.get<int64_t>());
        }
        else
        {
            std::string text = value.get<std::string>();
            result =
                sqlite3_bind_text(raw, int(i + 1), text.data(), int(text.size()), SQLITE_TRANSIENT);
        }
        check(result == SQLITE_OK, "database parameter failed", 500);
    }
    Json rows = Json::array();
    int result;
    while ((result = sqlite3_step(raw)) == SQLITE_ROW)
    {
        Json row = Json::object();
        for (int c = 0; c < sqlite3_column_count(raw); ++c)
        {
            std::string name = sqlite3_column_name(raw, c);
            if (sqlite3_column_type(raw, c) == SQLITE_NULL)
            {
                row[name] = nullptr;
            }
            else if (sqlite3_column_type(raw, c) == SQLITE_INTEGER)
            {
                row[name] = sqlite3_column_int64(raw, c);
            }
            else
            {
                auto *text = sqlite3_column_text(raw, c);
                row[name] = std::string(reinterpret_cast<const char *>(text),
                                        size_t(sqlite3_column_bytes(raw, c)));
            }
        }
        rows.push_back(row);
    }
    if (result == SQLITE_CONSTRAINT)
    {
        throw Error(409, "Asset is referenced by a channel; reassign or remove that channel first");
    }
    check(result == SQLITE_DONE, "database operation failed", 500);
    return rows;
}

void Store::execute(const std::string &sql, const std::vector<Json> &values)
{
    query(sql, values);
}

// Warehouse assets and their channel references.
Json Store::assets(const std::string &kind) const
{
    return decode_rows(query("SELECT * FROM assets WHERE kind=? ORDER BY created_at DESC", {kind}),
                       "info_json");
}

Json Store::asset(const std::string &id) const
{
    auto rows = decode_rows(query("SELECT * FROM assets WHERE id=?", {id}), "info_json");
    check(!rows.empty(), "Asset not found", 404);
    return rows[0];
}

void Store::add_asset(const Json &a)
{
    execute("INSERT INTO assets VALUES(?,?,?,?,?,?,?)",
            {a.at("id"), a.at("kind"), a.at("name"), a.at("status"), a.at("path"),
             a.at("info").dump(), a.at("created_at")});
}

void Store::update_asset_info(const std::string &id, const std::string &status, const Json &info)
{
    asset(id);
    execute("UPDATE assets SET status=?,info_json=? WHERE id=?", {status, info.dump(), id});
}

void Store::rename_asset(const std::string &id, const std::string &name)
{
    asset(id);
    execute("UPDATE assets SET name=? WHERE id=?", {name, id});
}

Json Store::references(const std::string &id) const
{
    return query("SELECT id,name FROM channels WHERE video_id=? OR model_id=?", {id, id});
}

void Store::remove_asset(const std::string &id)
{
    check(references(id).empty(), "Asset is assigned to a channel; reassign it before deletion",
          409);
    execute("DELETE FROM assets WHERE id=?", {id});
}

// Saved channel configuration.
Json Store::channels() const
{
    return decode_rows(query("SELECT * FROM channels ORDER BY created_at", {}), "config_json");
}

Json Store::channel(const std::string &id) const
{
    auto rows = decode_rows(query("SELECT * FROM channels WHERE id=?", {id}), "config_json");
    check(!rows.empty(), "Channel not found", 404);
    return rows[0];
}

void Store::save_channel(const Json &c)
{
    execute("INSERT INTO channels VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
            "name=excluded.name,video_id=excluded.video_id,model_id=excluded.model_id,config_json="
            "excluded.config_json",
            {c.at("id"), c.at("name"), c.value("video_id", Json()), c.value("model_id", Json()),
             c.at("config").dump(), c.at("created_at")});
}

void Store::remove_channel(const std::string &id)
{
    channel(id);
    execute("DELETE FROM channels WHERE id=?", {id});
}

// Upload and validation jobs.
Json Store::jobs() const
{
    auto rows =
        decode_rows(query("SELECT * FROM jobs ORDER BY created_at DESC LIMIT 100"), "detail_json");
    Json out = Json::array();
    for (auto &row : rows)
    {
        out.push_back(row["detail"]);
    }
    return out;
}

Json Store::job(const std::string &id) const
{
    auto rows = query("SELECT detail_json FROM jobs WHERE id=?", {id});
    check(!rows.empty(), "Upload not found", 404);
    return Json::parse(rows[0]["detail_json"].get<std::string>());
}

void Store::save_job(const Json &j)
{
    execute("INSERT INTO jobs VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
            "state=excluded.state,detail_json=excluded.detail_json",
            {j.at("id"), j.at("kind"), j.at("state"), j.dump(), j.at("created_at")});
}
} // namespace nvr::web
