#ifndef SQLITE_STORAGE_H
#define SQLITE_STORAGE_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <sqlite3.h>

class SQLiteStorage {
private:
    static std::mutex mutex_;
    static std::unique_ptr<SQLiteStorage> instance;
    sqlite3* db;
    bool isConnected;

    SQLiteStorage();
    void initDatabase();

public:
    static SQLiteStorage& getInstance();
    ~SQLiteStorage();

    void storeTradeData(const std::string& symbol, double rate);
    void storeTradeGroup(const std::string& exchangeId, const std::string& symbol,
                        const std::string& spotOrderId, const std::string& futuresOrderId,
                        int leverage);
    std::vector<std::string> getActiveTradeGroups();
    bool isConnectionValid() const;
};

#endif // SQLITE_STORAGE_H

