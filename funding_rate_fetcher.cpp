#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <chrono>
#include <hiredis/hiredis.h>
#include <json/json.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <fstream>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <mutex>
#include <memory>
#include <sstream>
#include <iomanip>
#include "config.h"
#include "exchange/bybit_api.h"

std::vector<std::string> splitString(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    size_t prev = 0, pos = 0;
    do {
        pos = str.find(delimiter, prev);
        if (pos == std::string::npos) pos = str.length();
        std::string token = str.substr(prev, pos-prev);
        if (!token.empty()) tokens.push_back(token);
        prev = pos + delimiter.length();
    } while (pos < str.length() && prev < str.length());
    return tokens;
}

// 资金费率获取模块
class FundingRateFetcher {
private:
    BybitAPI& api;
    int topPairsCount;

public:
    FundingRateFetcher(BybitAPI& api) : 
        api(api),
        topPairsCount(Config::getInstance().getTopPairsCount()) {}

    std::vector<std::pair<std::string, double>> getTopFundingRates() {
        auto rates = api.getFundingRates();
        std::sort(rates.begin(), rates.end(), 
                 [](auto& a, auto& b) { return std::abs(a.second) > std::abs(b.second); });
        
        // 只保留前X個
        if (rates.size() > topPairsCount) {
            rates.resize(topPairsCount);
        }
        return rates;
    }
};

// 数据存储模块
class SQLiteStorage {
private:
    static std::mutex mutex_;
    static std::unique_ptr<SQLiteStorage> instance;
    sqlite3* db;
    bool isConnected;

    SQLiteStorage() : db(nullptr), isConnected(false) {
        initDatabase();
    }

public:
    static SQLiteStorage& getInstance() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance) {
            instance.reset(new SQLiteStorage());
        }
        return *instance;
    }

    void initDatabase() {
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
        
        isConnected = true;
    }

    ~SQLiteStorage() {
        if (db) {
            sqlite3_close(db);
        }
    }

    void storeTradeData(const std::string& symbol, const double rate) {
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

    bool isConnectionValid() const {
        return isConnected && db != nullptr;
    }

    void storeTradeGroup(const std::string& exchangeId, const std::string& symbol,
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

    std::vector<std::string> getActiveTradeGroups() {
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
};

std::mutex SQLiteStorage::mutex_;
std::unique_ptr<SQLiteStorage> SQLiteStorage::instance;

// 交易模块
class TradingModule {
private:
    static std::mutex mutex_;
    static std::unique_ptr<TradingModule> instance;
    BybitAPI& api;
    SQLiteStorage& storage;

    TradingModule() : 
        api(BybitAPI::getInstance()),
        storage(SQLiteStorage::getInstance()) {}

public:
    static TradingModule& getInstance() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance) {
            instance.reset(new TradingModule());
        }
        return *instance;
    }

    std::vector<std::pair<std::string, double>> getTopFundingRates() {
        FundingRateFetcher fetcher(api);
        auto rates = fetcher.getTopFundingRates();
        
        std::cout << "\n=== 當前資金費率排名 ===" << std::endl;
        for (const auto& [symbol, rate] : rates) {
            std::cout << symbol << ": " << (rate * 100) << "%" << std::endl;
        }
        
        return rates;
    }

    void closeTradeGroup(const std::string& group) {
        auto parts = splitString(group, ":");
        if (parts.size() >= 2) {
            std::string symbol = parts[1];
            api.closePosition(symbol);
        }
    }

    void executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates) {
        double totalEquity = api.getTotalEquity();
        if (totalEquity <= 0) {
            std::cout << "總資金為0，無法進行沖交易" << std::endl;
            return;
        }

        double perPairAmount = totalEquity / 10;  // 每個幣對使用總資金的1/10
        
        for (const auto& [symbol, rate] : topRates) {
            double spotPrice = api.getSpotPrice(symbol);
            if (spotPrice <= 0) continue;

            double spotQty = perPairAmount / spotPrice;
            int leverage = Config::getInstance().getDefaultLeverage();
            double contractQty = spotQty * leverage;

            std::cout << "\n執行對沖交易: " << symbol << std::endl;
            std::cout << "現貨數量: " << spotQty << std::endl;
            std::cout << "合約數量: " << contractQty << std::endl;

            bool spotOrderSuccess = api.createSpotOrder(symbol, "Buy", spotQty);
            Json::Value contractOrder = api.createOrder(symbol, "Sell", contractQty);
            bool contractOrderSuccess = contractOrder["retCode"].asInt() == 0;

            if (spotOrderSuccess && contractOrderSuccess) {
                std::string spotOrderId = "SPOT_" + std::to_string(time(nullptr));
                std::string futuresOrderId = contractOrder["result"]["orderId"].asString();
                
                storage.storeTradeGroup(
                    "BYBIT", symbol, spotOrderId, futuresOrderId, leverage
                );
                std::cout << "對沖交易成功: " << symbol << std::endl;
            } else {
                if (spotOrderSuccess) {
                    api.createSpotOrder(symbol, "Sell", spotQty);
                }
                if (contractOrderSuccess) {
                    api.closePosition(symbol);
                }
                std::cout << "對沖交易失敗，已平倉: " << symbol << std::endl;
            }
        }
    }
};

std::mutex TradingModule::mutex_;
std::unique_ptr<TradingModule> TradingModule::instance;

// 调度器
void scheduleTask() {
    while (true) {
        try {
            auto& api = BybitAPI::getInstance();
            auto& storage = SQLiteStorage::getInstance();
            auto& trader = TradingModule::getInstance();

            // 檢查��金
            double totalEquity = api.getTotalEquity();
            if (totalEquity <= 0) {
                std::cout << "總資金為0，跳過本次交易" << std::endl;
                continue;
            }

            // 獲取並執行交易
            auto topRates = trader.getTopFundingRates();
            
            // 關閉不在前X名的持倉
            auto activeGroups = storage.getActiveTradeGroups();
            for (const auto& group : activeGroups) {
                // 解析交易組 ID
                auto parts = splitString(group, ":");
                std::string symbol = parts[1];
                
                bool inTopRates = false;
                for (const auto& [topSymbol, rate] : topRates) {
                    if (symbol == topSymbol) {
                        inTopRates = true;
                        break;
                    }
                }
                
                if (!inTopRates) {
                    trader.closeTradeGroup(group);
                }
            }

            // 執行新的交易
            trader.executeHedgeStrategy(topRates);

            // 存儲資金費率數據
            for (const auto& [symbol, rate] : topRates) {
                storage.storeTradeData(symbol, rate);
            }

        } catch (const std::exception& e) {
            std::cerr << "錯誤: " << e.what() << std::endl;
        }

        // 等待一小時
        std::this_thread::sleep_for(
            std::chrono::hours(Config::getInstance().getCheckIntervalHours())
        );
    }
}

int main() {
    scheduleTask();
    return 0;
}
