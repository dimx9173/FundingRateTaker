#ifndef TRADING_MODULE_H
#define TRADING_MODULE_H

#include "exchange/exchange_interface.h"
#include "storage/sqlite_storage.h"
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

std::vector<std::string> splitString(const std::string& str, const std::string& delimiter);

class TradingModule {
private:
    static std::mutex mutex_;
    static std::unique_ptr<TradingModule> instance;
    IExchange& exchange;
    SQLiteStorage& storage;

    TradingModule(IExchange& exchange);
    double calculatePositionSize(const std::string& symbol, double rate);
    bool checkTotalPositionLimit();

public:
    static TradingModule& getInstance(IExchange& exchange);
    std::vector<std::pair<std::string, double>> getTopFundingRates();
    void closeTradeGroup(const std::string& group);
    void executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates);
};

#endif // TRADING_MODULE_H
