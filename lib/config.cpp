#include "config.h"
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
    return config["bybit"]["api_key"].asString();
}

std::string Config::getBybitApiSecret() const {
    return config["bybit"]["api_secret"].asString();
}

std::string Config::getBybitBaseUrl() const {
    return config["bybit"]["base_url"].asString();
}

int Config::getDefaultLeverage() const {
    return config["bybit"]["default_leverage"].asInt();
}

int Config::getCheckIntervalHours() const {
    return config["trading"]["check_interval_hours"].asInt();
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