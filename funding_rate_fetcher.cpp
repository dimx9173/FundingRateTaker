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
#include "include/config.h"
#include "exchange/bybit_api.h"
#include "exchange/exchange_factory.h"


// 建議添加專門的日誌類
class Logger {
    void info(const std::string& message);
    void error(const std::string& message);
    void warning(const std::string& message);
};

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

// 资金费率获模块
class FundingRateFetcher {
private:
    IExchange& exchange;
    int topPairsCount;

public:
    FundingRateFetcher(IExchange& exchange) : 
        exchange(exchange),
        topPairsCount(Config::getInstance().getTopPairsCount()) {}

    std::vector<std::pair<std::string, double>> getTopFundingRates() {
        auto rates = exchange.getFundingRates();
        std::sort(rates.begin(), rates.end(), 
                 [](auto& a, auto& b) { return std::abs(a.second) > std::abs(b.second); });
        
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
    IExchange& exchange;
    SQLiteStorage& storage;

    TradingModule(IExchange& exchange) : 
        exchange(exchange),
        storage(SQLiteStorage::getInstance()) {}

    double calculatePositionSize(const std::string& symbol, double rate) {
        const Config& config = Config::getInstance();
        double totalInvestment = config.getTotalInvestment();
        double availableEquity = exchange.getTotalEquity();
        
        if (availableEquity <= 0 || availableEquity < totalInvestment * 0.1) {
            std::cout << "可用資金不足" << std::endl;
            return 0.0;
        }

        double basePosition = totalInvestment / config.getTopPairsCount();
        
        double adjustedPosition = basePosition;
        if (config.getPositionScaling()) {
            double scalingFactor = config.getScalingFactor();
            adjustedPosition = basePosition * (1 + std::abs(rate) * scalingFactor);
        }

        adjustedPosition = std::min(adjustedPosition, 
                                  totalInvestment * config.getMaxSinglePositionRisk());
        adjustedPosition = std::max(adjustedPosition, 
                                  config.getMinPositionSize() * exchange.getSpotPrice(symbol));
        adjustedPosition = std::min(adjustedPosition, 
                                  config.getMaxPositionSize() * exchange.getSpotPrice(symbol));

        return adjustedPosition;
    }

    bool checkTotalPositionLimit() {
        double totalPositionValue = 0.0;
        auto positions = exchange.getPositions();
        
        for (const auto& pos : positions["result"]["list"]) {
            totalPositionValue += std::stod(pos["positionValue"].asString());
        }

        return totalPositionValue < 
               (Config::getInstance().getTotalInvestment() * 
                Config::getInstance().getMaxTotalPosition());
    }

public:
    static TradingModule& getInstance(IExchange& exchange) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instance) {
            instance.reset(new TradingModule(exchange));
        }
        return *instance;
    }

    std::vector<std::pair<std::string, double>> getTopFundingRates() {
        FundingRateFetcher fetcher(exchange);
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
            exchange.closePosition(symbol);
        }
    }

    void executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates) {
        if (!checkTotalPositionLimit()) {
            std::cout << "已達到總倉位限制" << std::endl;
            return;
        }

        for (const auto& [symbol, rate] : topRates) {
            double positionSize = calculatePositionSize(symbol, rate);
            if (positionSize <= 0) continue;

            double spotPrice = exchange.getSpotPrice(symbol);
            if (spotPrice <= 0) continue;

            int leverage = Config::getInstance().getDefaultLeverage();
            double spotQty = positionSize / spotPrice;
            double contractQty = spotQty * leverage;

            std::cout << "\n執行對沖交易: " << symbol << std::endl;
            std::cout << "資金費率: " << (rate * 100) << "%" << std::endl;
            std::cout << "倉位大小: " << positionSize << " USDT" << std::endl;
            std::cout << "現貨數量: " << spotQty << std::endl;
            std::cout << "合約數量: " << contractQty << std::endl;

            bool spotOrderSuccess = exchange.createSpotOrder(symbol, "Buy", spotQty);
            Json::Value contractOrder = exchange.createOrder(symbol, "Sell", contractQty);
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
                    exchange.createSpotOrder(symbol, "Sell", spotQty);
                }
                if (contractOrderSuccess) {
                    exchange.closePosition(symbol);
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
            IExchange& exchange = ExchangeFactory::createExchange();
            auto& storage = SQLiteStorage::getInstance();
            auto& trader = TradingModule::getInstance(exchange);

            auto topRates = trader.getTopFundingRates();

            double totalEquity = exchange.getTotalEquity();
            if (totalEquity <= 0) {
                std::cout << "總資金為0，跳過本次交易" << std::endl;
                std::this_thread::sleep_for(
                    std::chrono::seconds(10)
                );
                continue;
            }

            
            auto activeGroups = storage.getActiveTradeGroups();
            for (const auto& group : activeGroups) {
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

            trader.executeHedgeStrategy(topRates);

            for (const auto& [symbol, rate] : topRates) {
                storage.storeTradeData(symbol, rate);
            }

        } catch (const std::exception& e) {
            std::cerr << "錯誤: " << e.what() << std::endl;
        }

        std::this_thread::sleep_for(
            std::chrono::minutes(Config::getInstance().getCheckIntervalMinutes())
        );
    }
}

int main() {
    scheduleTask();
    return 0;
}
