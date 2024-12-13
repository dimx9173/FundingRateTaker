#include "storage/sqlite_storage.h"
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <sqlite3.h>

std::mutex SQLiteStorage::mutex_;
std::unique_ptr<SQLiteStorage> SQLiteStorage::instance;

SQLiteStorage::SQLiteStorage() : db(nullptr), isConnected(false) {
    initDatabase();
}

SQLiteStorage& SQLiteStorage::getInstance() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!instance) {
        instance.reset(new SQLiteStorage());
    }
    return *instance;
}

void SQLiteStorage::initDatabase() {
    int rc = sqlite3_open("trading.db", &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return;
    }
    
    // 創表格（如果不存在）
    const char* sql = "CREATE TABLE IF NOT EXISTS trades ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                     "symbol TEXT NOT NULL,"
                     "rate REAL NOT NULL,"
                     "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
                     ");";
    char* errMsg = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return;
    }

    // 創建 trade_groups 表
    sql = "CREATE TABLE IF NOT EXISTS trade_groups ("
          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "exchange_id TEXT NOT NULL,"
          "symbol TEXT NOT NULL,"
          "spot_order_id TEXT NOT NULL,"
          "futures_order_id TEXT NOT NULL,"
          "leverage INTEGER NOT NULL,"
          "active INTEGER DEFAULT 1,"
          "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
          ");";
    
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return;
    }
    
    isConnected = true;
}

SQLiteStorage::~SQLiteStorage() {
    if (db) {
        sqlite3_close(db);
    }
}

void SQLiteStorage::storeTradeData(const std::string& symbol, const double rate) {
    if (!isConnected || !db) {
        std::cerr << "Cannot store data: SQLite not connected" << std::endl;
        return;
    }

    const char* sql = "INSERT INTO trades (symbol, rate) VALUES (?, ?);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, rate);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to insert data: " << sqlite3_errmsg(db) << std::endl;
    }

    sqlite3_finalize(stmt);
}

bool SQLiteStorage::isConnectionValid() const {
    return isConnected && db != nullptr;
}

void SQLiteStorage::storeTradeGroup(const std::string& exchangeId, const std::string& symbol,
                                  const std::string& spotOrderId, const std::string& futuresOrderId,
                                  int leverage) {
    if (!isConnected || !db) {
        std::cerr << "Cannot store trade group: SQLite not connected" << std::endl;
        return;
    }

    const char* sql = "INSERT INTO trade_groups (exchange_id, symbol, spot_order_id, "
                     "futures_order_id, leverage) VALUES (?, ?, ?, ?, ?);";
    
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    sqlite3_bind_text(stmt, 1, exchangeId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, symbol.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, spotOrderId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, futuresOrderId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, leverage);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to insert trade group: " << sqlite3_errmsg(db) << std::endl;
    }

    sqlite3_finalize(stmt);
}

std::vector<std::string> SQLiteStorage::getActiveTradeGroups() {
    std::vector<std::string> groups;
    const char* sql = "SELECT exchange_id || ':' || symbol || ':' || "
                     "spot_order_id || '_' || futures_order_id || '_' || leverage "
                     "FROM trade_groups WHERE active = 1;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            groups.push_back((const char*)sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return groups;
}
