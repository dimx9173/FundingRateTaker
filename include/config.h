#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <json/json.h>

class Config {
private:
    static std::unique_ptr<Config> instance;
    static std::mutex mutex_;
    Json::Value config;
    Json::Value pair_list;
    
    Config();
    void loadConfigFiles();
    void loadFile(const std::string& filename, Json::Value& target);

public:
    static Config& getInstance();
    
    // 交易所相關配置
    std::string getPreferredExchange() const;
    bool isExchangeEnabled(const std::string& exchange) const;
    std::string getBybitApiKey() const;
    std::string getBybitApiSecret() const;
    std::string getBybitBaseUrl() const;
    int getDefaultLeverage() const;
    bool isSpotMarginTradingEnabled() const;
    
    // 資金費率相關配置
    bool getReverseContractFundingRate() const;
    bool getUseCoinMarketCap() const;
    std::string getCMCApiKey() const;
    int getCMCTopCount() const;
    std::string getCMCSortBy() const;
    std::vector<std::string> getSettlementTimesUTC() const;
    int getPreSettlementMinutes() const;
    std::vector<int> getFundingPeriods() const;
    std::vector<double> getFundingWeights() const;
    int getFundingHistoryDays() const;
    int getFundingHoldingDays() const;
    double getMinScalingRate() const;
    double getMaxScalingRate() const;
    bool getPositionScaling() const;
    double getScalingFactor() const;

    // 交易相關配置
    int getCheckIntervalMinutes() const;
    int getTopPairsCount() const;
    std::vector<std::string> getTradingPairs() const;
    double getMinPositionValue() const;
    double getMaxPositionValue() const;
    std::vector<std::string> getUnsupportedSymbols() const;
}; 