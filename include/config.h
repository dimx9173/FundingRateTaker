#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <json/json.h>
#include <memory>
#include <mutex>

class Config {
private:
    static std::mutex mutex_;
    static std::unique_ptr<Config> instance;
    Json::Value config;
    Json::Value pair_list;

    Config();
    void loadConfigFiles();
    void loadFile(const std::string& filename, Json::Value& target);

public:
    static Config& getInstance();

    std::string getBybitApiKey() const;
    std::string getBybitApiSecret() const;
    std::string getBybitBaseUrl() const;
    int getDefaultLeverage() const;
    int getTopPairsCount() const;
    std::vector<std::string> getTradingPairs() const;
    double getMinTradeAmount() const;
    double getMaxTradeAmount() const;
    int getMaxPositions() const;
    double getStopLossPercentage() const;

    double getTotalInvestment() const;
    double getMinPositionSize() const;
    double getMaxPositionSize() const;
    double getMaxTotalPosition() const;
    double getMaxSinglePositionRisk() const;
    bool getPositionScaling() const;
    double getScalingFactor() const;
    int getCheckIntervalMinutes() const;

    std::string getPreferredExchange() const {
        return config["preferred_exchange"].asString();
    }
};

#endif // CONFIG_H 