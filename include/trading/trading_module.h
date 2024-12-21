#ifndef TRADING_MODULE_H
#define TRADING_MODULE_H

#include "exchange/exchange_interface.h"
#include "storage/sqlite_storage.h"
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "logger.h"

std::vector<std::string> splitString(const std::string& str, const std::string& delimiter);

class TradingModule {
private:
    static std::mutex mutex_;
    static std::unique_ptr<TradingModule> instance;
    IExchange& exchange;
    SQLiteStorage& storage;
    Logger logger;
 std::vector<std::pair<std::string, double>> cachedFundingRates;
    std::chrono::system_clock::time_point lastFundingUpdate;
    TradingModule(IExchange& exchange);
    struct BalanceCheckResult {
        bool needBalance;
        double priceDiff;
        double depthImpact;
        double estimatedCost;
        double expectedProfit;
    };
    double calculatePositionSize(const std::string& symbol, double rate);
    bool checkTotalPositionLimit();
    bool isNearSettlement();
    std::vector<std::string> getCurrentPositionSymbols();
    bool shouldClosePosition(const std::string& symbol, 
                           const std::vector<std::pair<std::string, double>>& topRates);
    
    double adjustSpotPrecision(double quantity, const std::string& symbol);
    double adjustContractPrecision(double quantity, const std::string& symbol);
    double getMinOrderSize(const std::string& symbol);
    double getMinOrderValue(const std::string& symbol);
    double calculateTotalInvestment(
        const std::map<std::string, std::pair<double, double>>& positions, 
        IExchange& exchange);
    double calculateAdjustedPosition(double basePosition, double rate);
    void updateUnsupportedSymbols(const std::string& symbol);
    std::map<std::string, std::pair<double, double>> getCurrentPositionSizes();
    void handleExistingPositions(std::map<std::string, std::pair<double, double>>& positionSizes,
                                const std::vector<std::pair<std::string, double>>& topRates);
    void balancePositions(const std::vector<std::pair<std::string, double>>& topRates,
                          std::map<std::string, std::pair<double, double>>& positionSizes);
    void handleError(const std::string& symbol, const std::string& error);
    BalanceCheckResult checkPositionBalance(const std::string& symbol, 
                                          double spotSize, 
                                          double contractSize);
    double calculateDepthImpact(const Json::Value& orderbook, double size);
    double calculateRebalanceCost(const std::string& symbol, double size, bool isSpot, const Json::Value& orderbook);
    double calculateExpectedProfit(double size, double fundingRate);
    bool createSpotOrderIncludeFee(const std::string& symbol, const std::string& side, double qty);
    bool executeHedgePosition(
        const std::string& symbol,
        double targetValue,
        const BalanceCheckResult& balanceCheck,
        std::map<std::string, std::pair<double, double>>& positionSizes);
    double calculateTotalPositionValue(
        const std::map<std::string, std::pair<double, double>>& positions,
        bool positionsIsSize,
        const std::map<std::string, std::pair<double, double>>* prices);
    std::vector<std::string> getSymbolsByCMC(int topCount);
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    void displayPositionSizes(const std::map<std::string, std::pair<double, double>>& positionSizes);
public:
    static TradingModule& getInstance(IExchange& exchange);
    std::vector<std::pair<std::string, double>> getTopFundingRates();
    void closeTradeGroup(const std::string& group);
    void executeHedgeStrategy();
    static void resetInstance() {
        std::lock_guard<std::mutex> lock(mutex_);
        instance.reset();
    }
    void displayPositions();
};

#endif // TRADING_MODULE_H
