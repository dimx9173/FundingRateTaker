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
    Json::Value getExchangeConfig(const std::string& exchange) const;
    std::string getBybitApiKey() const;
    std::string getBybitApiSecret() const;
    std::string getBybitBaseUrl() const;
    int getDefaultLeverage() const;
    
    // 資金費率相關配置
    std::vector<std::string> getSettlementTimesUTC() const;
    int getPreSettlementMinutes() const;
    std::vector<int> getFundingPeriods() const;
    std::vector<double> getFundingWeights() const;
    int getFundingHistoryDays() const;
    
    // 交易相關配置
    int getCheckIntervalMinutes() const;
    int getTopPairsCount() const;
    std::vector<std::string> getTradingPairs() const;
    double getMinTradeAmount() const;
    double getMaxTradeAmount() const;
    int getMaxPositions() const;
    double getStopLossPercentage() const;
    double getTotalInvestment() const;
    
    // 倉位管理相關配置
    double getMinPositionSize() const;
    double getMaxPositionSize() const;
    double getMaxTotalPosition() const;
    double getMaxSinglePositionRisk() const;
    bool getPositionScaling() const;
    double getScalingFactor() const;
}; 