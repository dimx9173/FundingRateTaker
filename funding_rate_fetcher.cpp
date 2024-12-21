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
#include "trading/trading_module.h"
#include "storage/sqlite_storage.h"
#include "logger.h"

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

bool isNearSettlement() {
    auto now = std::chrono::system_clock::now();
    auto utc_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm = *std::gmtime(&utc_time);
    
    int current_minutes = utc_tm.tm_hour * 60 + utc_tm.tm_min;
    auto settlement_times = Config::getInstance().getSettlementTimesUTC();
    int pre_minutes = Config::getInstance().getPreSettlementMinutes();
    
    for (const auto& time_str : settlement_times) {
        std::istringstream ss(time_str);
        int hour, minute;
        char delimiter;
        ss >> hour >> delimiter >> minute;
        
        int settlement_minutes = hour * 60 + minute;
        int diff = std::abs(current_minutes - settlement_minutes);
        
        // 考慮跨日情況
        if (diff > 720) { // 12小時 = 720分鐘
            diff = 1440 - diff; // 24小時 = 1440分鐘
        }
        
        if (diff <= pre_minutes) {
            return true;
        }
    }
    return false;
}

// 调度器
void scheduleTask() {
    Logger logger;
    while (true) {
        try {
            IExchange& exchange = ExchangeFactory::createExchange();
            auto& trader = TradingModule::getInstance(exchange);
            trader.displayPositions();
            trader.executeHedgeStrategy(); 
            trader.displayPositions();
            logger.info("對沖策略執行完成");
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
