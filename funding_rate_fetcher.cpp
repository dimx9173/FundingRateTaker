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

// 建議添加專門的日誌類
class Logger {
public:
    void info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }
    void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
    void warning(const std::string& message) {
        std::cerr << "[WARNING] " << message << std::endl;
    }
};

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

// 调度器
void scheduleTask() {
    Logger logger;
    while (true) {
        try {
            IExchange& exchange = ExchangeFactory::createExchange();
            auto& trader = TradingModule::getInstance(exchange);
            auto topRates = trader.getTopFundingRates(); 
            trader.executeHedgeStrategy(topRates); 
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
