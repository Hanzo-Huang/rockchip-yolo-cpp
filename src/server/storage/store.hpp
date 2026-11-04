#pragma once

#include "server/services/common.hpp"
#include <sqlite3.h>

namespace nvr::web
{
class Store
{
    sqlite3 *db_ = nullptr;
    mutable std::recursive_mutex mutex_;

    Json query(const std::string &sql, const std::vector<Json> &values = {}) const;
    void execute(const std::string &sql, const std::vector<Json> &values = {});

public:
    explicit Store(const fs::path &database);
    ~Store();
    Store(const Store &) = delete;

    // Warehouse assets and their channel references.
    Json assets(const std::string &kind) const;
    Json asset(const std::string &id) const;
    void add_asset(const Json &asset);
    void update_asset_info(const std::string &id, const std::string &status, const Json &info);
    void rename_asset(const std::string &id, const std::string &name);
    void remove_asset(const std::string &id);
    Json references(const std::string &id) const;

    // Saved channel configuration.
    Json channels() const;
    Json channel(const std::string &id) const;
    void save_channel(const Json &channel);
    void remove_channel(const std::string &id);

    // Upload and validation jobs.
    Json jobs() const;
    Json job(const std::string &id) const;
    void save_job(const Json &job);
};
} // namespace nvr::web
