#include "include/config.h"
#include <fstream>
#include <stdexcept>

std::mutex Config::mutex_;
std::unique_ptr<Config> Config::instance;

Config::Config() {
    loadConfigFiles();
}

void Config::loadConfigFiles() {
    loadFile("config.json", config);
    loadFile("pair_list.json", pair_list);
}

void Config::loadFile(const std::string& filename, Json::Value& target) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("無法打開文件: " + filename);
    }

    Json::Reader reader;
    if (!reader.parse(file, target)) {
        throw std::runtime_error("解析失敗: " + filename);
    }
}

Config& Config::getInstance() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!instance) {
        instance.reset(new Config());
    }
    return *instance;
}

std::string Config::getBybitApiKey() const {
    return config["exchanges"]["bybit"]["api_key"].asString();
}

std::string Config::getBybitApiSecret() const {
    return config["exchanges"]["bybit"]["api_secret"].asString();
}

std::string Config::getBybitBaseUrl() const {
    return config["exchanges"]["bybit"]["base_url"].asString();
}

int Config::getDefaultLeverage() const {
    return config["exchanges"]["bybit"]["default_leverage"].asInt();
}

int Config::getCheckIntervalMinutes() const {
    return config["trading"]["check_interval_minutes"].asInt();
}

int Config::getTopPairsCount() const {
    return config["top_pairs_count"].asInt();
}

std::vector<std::string> Config::getTradingPairs() const {
    std::vector<std::string> pairs;
    const Json::Value& pairList = pair_list["pair_list"];
    for (const auto& pair : pairList) {
        pairs.push_back(pair.asString());
    }
    return pairs;
}

double Config::getMinTradeAmount() const {
    return config["trading"]["min_trade_amount"].asDouble();
}

double Config::getMaxTradeAmount() const {
    return config["trading"]["max_trade_amount"].asDouble();
}

int Config::getMaxPositions() const {
    return config["trading"]["max_positions"].asInt();
}

double Config::getStopLossPercentage() const {
    return config["trading"]["stop_loss_percentage"].asDouble();
}

double Config::getTotalInvestment() const {
    return config["trading"]["total_investment"].asDouble();
}

double Config::getMinPositionSize() const {
    return config["trading"]["position_limits"]["min_position_size"].asDouble();
}

double Config::getMaxPositionSize() const {
    return config["trading"]["position_limits"]["max_position_size"].asDouble();
}

double Config::getMaxTotalPosition() const {
    return config["trading"]["position_limits"]["max_total_position"].asDouble();
}

double Config::getMaxSinglePositionRisk() const {
    return config["trading"]["risk_management"]["max_single_position_risk"].asDouble();
}

bool Config::getPositionScaling() const {
    return config["trading"]["risk_management"]["position_scaling"].asBool();
}

double Config::getScalingFactor() const {
    return config["trading"]["risk_management"]["scaling_factor"].asDouble();
}

std::vector<int> Config::getFundingPeriods() const {
    std::vector<int> periods;
    const Json::Value& periodsArray = config["trading"]["funding_rate_scoring"]["periods"];
    for (const auto& period : periodsArray) {
        periods.push_back(period.asInt());
    }
    return periods;
}

std::vector<double> Config::getFundingWeights() const {
    std::vector<double> weights;
    const Json::Value& weightsArray = config["trading"]["funding_rate_scoring"]["weights"];
    for (const auto& weight : weightsArray) {
        weights.push_back(weight.asDouble());
    }
    return weights;
}

std::string Config::getPreferredExchange() const {
    return config["preferred_exchange"].asString();
}

bool Config::isExchangeEnabled(const std::string& exchange) const {
    std::string lowerExchange = exchange;
    std::transform(lowerExchange.begin(), lowerExchange.end(), lowerExchange.begin(), ::tolower);
    return config["exchanges"][lowerExchange]["enabled"].asBool();
}

std::vector<std::string> Config::getSettlementTimesUTC() const {
    std::vector<std::string> times;
    const Json::Value& timesArray = config["trading"]["funding_rate_scoring"]["settlement_times_utc"];
    for (const auto& time : timesArray) {
        times.push_back(time.asString());
    }
    return times;
}

int Config::getPreSettlementMinutes() const {
    return config["trading"]["funding_rate_scoring"]["pre_settlement_minutes"].asInt();
}

Json::Value Config::getExchangeConfig(const std::string& exchange) const {
    std::string lowerExchange = exchange;
    std::transform(lowerExchange.begin(), lowerExchange.end(), lowerExchange.begin(), ::tolower);
    return config["exchanges"][lowerExchange];
}

int Config::getFundingHistoryDays() const {
    return config["trading"]["funding_rate_scoring"]["history_days"].asInt();
} 