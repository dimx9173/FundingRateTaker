#ifndef TRADING_MODULE_H
#define TRADING_MODULE_H

#include "exchange/exchange_interface.h"
#include "storage/sqlite_storage.h"
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "../logger.h"

std::vector<std::string> splitString(const std::string& str, const std::string& delimiter);

class TradingModule {
private:
    static std::mutex mutex_;
    static std::unique_ptr<TradingModule> instance;
    IExchange& exchange;
    SQLiteStorage& storage;
    Logger logger;

    TradingModule(IExchange& exchange);
    double calculatePositionSize(const std::string& symbol, double rate);
    bool checkTotalPositionLimit();
    bool isNearSettlement();
    std::vector<std::string> getCurrentPositionSymbols();
    bool shouldClosePosition(const std::string& symbol, 
                           const std::vector<std::pair<std::string, double>>& topRates);
    
    double adjustPrecision(double quantity, const std::string& symbol);
    double getMinOrderSize(const std::string& symbol);
    double getMinOrderValue(const std::string& symbol);
    double calculateTotalInvestment(
        const std::map<std::string, std::pair<double, double>>& positions, 
        IExchange& exchange);
    double calculateAdjustedPosition(double basePosition, double rate);

public:
    static TradingModule& getInstance(IExchange& exchange);
    std::vector<std::pair<std::string, double>> getTopFundingRates();
    void closeTradeGroup(const std::string& group);
    void executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates);
    static void resetInstance() {
        std::lock_guard<std::mutex> lock(mutex_);
        instance.reset();
    }
};

#endif // TRADING_MODULE_H
