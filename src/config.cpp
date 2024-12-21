#include "include/config.h"
#include <fstream>
#include <stdexcept>

std::mutex Config::mutex_;
std::unique_ptr<Config> Config::instance;

Config::Config() {
    loadConfigFiles();
}

void Config::loadConfigFiles() {
    loadFile("config/config.json", config);
    loadFile("config/pair_list.json", pair_list);
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

int Config::getFundingHistoryDays() const {
    return config["trading"]["funding_rate_scoring"]["history_days"].asInt();
} 
// 在 Config 類的 public 部分添加：
double Config::getMinPositionValue() const { 
    return config["trading"]["min_position_value"].asDouble(); 
}

double Config::getMaxPositionValue() const { 
    return config["trading"]["max_position_value"].asDouble(); 
}

std::vector<std::string> Config::getUnsupportedSymbols() const {
    std::vector<std::string> pairs;
    const Json::Value& pairList = pair_list["unsupported_symbols"];
    for (const auto& pair : pairList) {
        pairs.push_back(pair.asString());
    }
    return pairs;
}

bool Config::isSpotMarginTradingEnabled() const {
    std::string lowerExchange = getPreferredExchange();
    std::transform(lowerExchange.begin(), lowerExchange.end(), lowerExchange.begin(), ::tolower);
    return config["exchanges"][lowerExchange]["spot_margin_trading"].asBool();
}

bool Config::getPositionScaling() const {
    return config["trading"]["position_scaling"].asBool();
}

double Config::getScalingFactor() const {
    return config["trading"]["scaling_factor"].asDouble();
}

double Config::getMinScalingRate() const {
    return config["trading"]["min_scaling_rate"].asDouble();
}

double Config::getMaxScalingRate() const {
    return config["trading"]["max_scaling_rate"].asDouble();
}

int Config::getFundingHoldingDays() const {
    return config["trading"]["funding_holding_days"].asInt();
}

bool Config::getUseCoinMarketCap() const {
    return config["trading"]["use_coin_market_cap"].asBool();
}

std::string Config::getCMCApiKey() const {
    return config["trading"]["cmc_api_key"].asString();
}

std::string Config::getCMCSortBy() const {
    return config["trading"]["cmc_sort_by"].asString();
}

int Config::getCMCTopCount() const {
    return config["trading"]["cmc_top_count"].asInt();
}

bool Config::getReverseContractFundingRate() const {
    return config["trading"]["reverse_contract_funding_rate"].asBool();
}