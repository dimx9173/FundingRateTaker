#include "trading/trading_module.h"
#include "config.h"
#include <iostream>
#include <ctime>
#include <iomanip>
#include <thread>
#include <chrono>
#include <set>
#include <fstream>
#include <sstream>
#include <curl/curl.h>

std::mutex TradingModule::mutex_;
std::unique_ptr<TradingModule> TradingModule::instance;

TradingModule::TradingModule(IExchange& exchange) : 
    exchange(exchange),
    storage(SQLiteStorage::getInstance()) {}

TradingModule& TradingModule::getInstance(IExchange& exchange) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!instance) {
        instance.reset(new TradingModule(exchange));
    }
    return *instance;
}

double TradingModule::calculatePositionSize(const std::string& symbol, double rate) {
    const Config& config = Config::getInstance();
    double availableEquity = exchange.getTotalEquity();
    
    // 檢查可用資金
    if (availableEquity <= 0) {
        std::cout << "可用資金不足" << std::endl;
        return 0.0;
    }

    // 獲取當前價格
    double currentPrice = exchange.getSpotPrice(symbol);
    if (currentPrice <= 0) {
        std::cout << "無法獲取" << symbol << "價格" << std::endl;
        return 0.0;
    }

    // 設置最小倉位價值（USDT）
    double minPositionValue = config.getMinPositionValue();
    double maxPositionValue = config.getMaxPositionValue();
    
    // 基礎倉位計算
    double basePosition = minPositionValue;
    
    // 根據資金費率調整倉位大小
    double adjustedPosition = basePosition;
    if (config.getPositionScaling()) {
        double scalingFactor = config.getScalingFactor();
        double minRate = config.getMinScalingRate();  // 新增最小縮放費率閾值
        double maxRate = config.getMaxScalingRate();  // 新增最大縮放費率閾值
        
        // 將費率限制在合理範圍內
        double normalizedRate = std::clamp(std::abs(rate), minRate, maxRate);
        
        // 使用對數函數進行平滑縮放
        double scalingMultiplier = 1.0 + std::log1p(normalizedRate * scalingFactor);
        
        // 應用縮放
        adjustedPosition = basePosition * scalingMultiplier;
        
        // 限制最大倉位價值
        adjustedPosition = std::min(adjustedPosition, maxPositionValue);
        
        std::stringstream ss;
        ss << "倉位調整詳情: "
           << "原始倉位=" << basePosition
           << ", 費率=" << rate
           << ", 標準化費率=" << normalizedRate
           << ", 縮放倍數=" << scalingMultiplier
           << ", 調整後倉位=" << adjustedPosition;
        logger.debug(ss.str());
    }

    // 計算數量並調整精度
    double quantity = adjustedPosition / currentPrice;
    quantity = adjustSpotPrecision(quantity, symbol);
    
    // 確保最終倉位價值小於最小要求
    double finalValue = quantity * currentPrice;
    if (finalValue < minPositionValue) {
        return 0.0;  // 放棄該交易
    }

    return finalValue;
}

std::vector<std::pair<std::string, double>> TradingModule::getTopFundingRates() {
    // 檢查是否需要更新資金費率
    bool needUpdate = cachedFundingRates.empty() || isNearSettlement();
    
    if (!needUpdate) {
        logger.info("使用緩存的資金費率數據");
        return cachedFundingRates;
    }
    
    logger.info("重新獲取資金費率數據...");
    
    int topCount = Config::getInstance().getTopPairsCount();
    
    const Config& config = Config::getInstance();
    bool useCoinMarketCap = config.getUseCoinMarketCap();
    int cmcTopCount = config.getCMCTopCount();
    std::vector<std::string> symbols;
    
    if (useCoinMarketCap) {
        //使用CMC的幣種 
        symbols = getSymbolsByCMC(cmcTopCount);
    } else {
        //使用配置中的所有交易對
        symbols = config.getTradingPairs();
    }
    
    
    // 獲取不支持的交易對, 並從symbols中移除
    auto unsupportedList = config.getUnsupportedSymbols();
    std::set<std::string> unsupportedSymbols(unsupportedList.begin(), unsupportedList.end());
    symbols.erase(std::remove_if(symbols.begin(), symbols.end(),
        [&unsupportedSymbols](const std::string& symbol) {
            return unsupportedSymbols.find(symbol) != unsupportedSymbols.end();
        }), symbols.end());
    
    //移除重複
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());

    // 獲取配置參數
    auto periods = Config::getInstance().getFundingPeriods();  // [a, b, c]
    auto weights = Config::getInstance().getFundingWeights();  // [o, p, q]
    
    if (periods.size() != weights.size()) {
        logger.error("資金費率週期和權重配置不匹配");
        return {};
    }
    
    // 獲取資金費率數據
    std::vector<std::pair<std::string, std::vector<double>>> historicalRates;
    try {
        historicalRates = exchange.getFundingHistory(symbols);
        if (historicalRates.empty()) {
            logger.warning("沒有獲取到任何資金費率數據");
            return {};
        }
    } catch (const std::exception& e) {
        logger.error("獲取資金費率失敗: " + std::string(e.what()));
        return {};
    }
    
    
    
    // 計算加權分數
    std::vector<std::pair<std::string, double>> weightedRates;
    for (const auto& [symbol, rates] : historicalRates) {

        if (unsupportedSymbols.find(symbol) != unsupportedSymbols.end()) {
            logger.info("跳過不支持的交易對: " + symbol);
            continue;
        }
        
        if (rates.empty()) {
            logger.warning("無效的資金費率數據: " + symbol);
            continue;
        }

        // 是否支援反向現貨合約資金費率, 即空付多收，借現貨賣做多合約（反向合約資金費率 = 現貨資金費率 * -1）
        if (!config.getReverseContractFundingRate()) {
            if (rates[0] < 0) {
                logger.info("不支援反向現貨合約資金費率，跳過資金費率為負值的幣種: " + symbol);
                continue;
            }
        }
        
        double weightedScore = 0.0;
        double totalWeight = 0.0;
        
        // 對每個週期計算加權平均
        for (size_t i = 0; i < periods.size(); i++) {
            int periodLimit = std::min(periods[i], static_cast<int>(rates.size()));
            double periodSum = 0.0;
            
            for (int j = 0; j < periodLimit; j++) {
                if (std::isfinite(rates[j])) {
                    periodSum += rates[j];
                }
            }
            
            if (periodLimit > 0) {
                double periodAvg = periodSum / periodLimit;
                weightedScore += periodAvg * weights[i];
                totalWeight += weights[i];
            }
        }
        
        if (totalWeight > 0) {
            double finalScore = weightedScore / totalWeight;
            // 檢查最新資金費率是否與finalScore相反
            if (rates.empty() || rates[0] * finalScore < 0) {
                logger.info("跳過資金費率與最後一個週期相反的幣種: " + symbol);
                continue;
            }
            weightedRates.emplace_back(symbol, finalScore);
        }
    }
    
    if (weightedRates.empty()) {
        logger.warning("計算後沒有可用的資金費率數據");
        return {};
    }
    
    // 按資金費率絕對值排序
    std::sort(weightedRates.begin(), weightedRates.end(),
        [](const auto& a, const auto& b) {
            return std::abs(a.second) > std::abs(b.second);
        });
    
    // 只保留前N個交易對
    if (topCount > 0 && weightedRates.size() > static_cast<size_t>(topCount)) {
        weightedRates.resize(topCount);
    }
    
    // 輸出排名結果
    std::cout << "\n=== 資金費率排名 ===" << std::endl;
    for (const auto& [symbol, rate] : weightedRates) {
        std::string direction = rate > 0 ? "多付空收" : "空付多收";
        
        // 找到該交易對的歷史費率
        auto it = std::find_if(historicalRates.begin(), historicalRates.end(),
            [&symbol](const auto& pair) { return pair.first == symbol; });
            
        // 計算年化報酬率 (每天3次結算，一年365天)
        double annualizedReturn = rate * 3 * 365 * 100;  // 轉換為百分比
            
        std::cout << std::left << std::setw(12) << symbol 
                 << ": " << std::setw(8) << std::fixed << std::setprecision(4) 
                 << (rate * 100) << "% " << std::setw(12) << direction
                 << " 年化: " << std::setw(8) << std::fixed << std::setprecision(2)
                 << annualizedReturn << "%";
        
        // 顯示各週期的平均分數
        if (it != historicalRates.end()) {
            const auto& rates = it->second;
            std::cout << " [";
            for (size_t i = 0; i < periods.size(); i++) {
                int periodLimit = std::min(periods[i], static_cast<int>(rates.size()));
                double periodSum = 0.0;
                int validCount = 0;
                
                for (int j = 0; j < periodLimit; j++) {
                    if (std::isfinite(rates[j])) {
                        periodSum += rates[j];
                        validCount++;
                    }
                }
                
                double periodAvg = validCount > 0 ? periodSum / validCount : 0.0;
                std::cout << std::fixed << std::setprecision(4) 
                         << (periodAvg * 100) << "%";
                         
                if (i < periods.size() - 1) {
                    std::cout << ", ";
                }
            }
            std::cout << "]";
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;
    
    // 更新緩存
    cachedFundingRates = weightedRates;
    lastFundingUpdate = std::chrono::system_clock::now();
    
    return cachedFundingRates;
}

void TradingModule::closeTradeGroup(const std::string& group) {
    auto parts = splitString(group, ":");
    if (parts.size() >= 2) {
        std::string symbol = parts[1];
        exchange.closePosition(symbol);
    }
}

void TradingModule::executeHedgeStrategy() {
    try {

        // 1. 獲取資金費率
        auto topRates = getTopFundingRates(); 

        logger.info("開始執行對衝策略...");

        // 2. 獲取當前倉位狀態
        auto positionSizes = getCurrentPositionSizes();
        if (positionSizes.empty()) {
            logger.info("無法獲取倉位信息，跳過本次執行");
            return;
        }
        
        // 3. 關閉不在topRates的現有倉位
        if (!positionSizes.empty()) {
            logger.info("開始關閉不在topRates的現有倉位");
            handleExistingPositions(positionSizes, topRates);
            displayPositionSizes(positionSizes);
        }
    
        // 4. 新建倉位並平衡現有倉位
        logger.info("開始平衡倉位...");
        balancePositions(topRates, positionSizes);
        
        logger.info("對衝策略執行完成");
        
    } catch (const std::exception& e) {
        logger.error("執行對衝策略時發生錯誤: " + std::string(e.what()));
    }
}

bool TradingModule::isNearSettlement() {
    auto now = std::chrono::system_clock::now();
    auto utc_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm = *std::gmtime(&utc_time);
    int current_minutes = utc_tm.tm_hour * 60 + utc_tm.tm_min;
    auto settlement_times = Config::getInstance().getSettlementTimesUTC();
    int pre_minutes = Config::getInstance().getPreSettlementMinutes();

    // 找到下一個結算時間
    for (const auto& time_str : settlement_times) {
        std::istringstream ss(time_str);
        int hour, minute;
        char delimiter;
        ss >> hour >> delimiter >> minute;
        int settlement_minutes = hour * 60 + minute;
        
        // 計算時間差（考慮跨日情況）
        int diff = settlement_minutes - current_minutes;
        if (diff < 0) {
            diff += 1440; // 24小時 = 1440分鐘
        }
        
        // 如果在預設時間範圍內
        if (diff <= pre_minutes) {
            logger.info("距離下次結算時間還有：" + std::to_string(diff) + "分鐘");
            return true;
        }
    }
    return false;
}

std::vector<std::string> TradingModule::getCurrentPositionSymbols() {
    std::vector<std::string> currentSymbols;
    
    // 獲取合約倉位
    auto positions = exchange.getPositions();
    if (positions.isObject() && positions["result"]["list"].isArray()) {
        const Json::Value& list = positions["result"]["list"];
        for (const auto& pos : list) {
            double size = std::stod(pos["size"].asString());
            if (size > 0) {
                currentSymbols.push_back(pos["symbol"].asString());
            }
        }
    }
    
    // 獲取現貨倉位
    auto spotBalances = exchange.getSpotBalances();
    if (spotBalances.isObject() && spotBalances["result"]["list"].isArray()) {
        const auto& list = spotBalances["result"]["list"][0]["coin"];
        if (list.isArray()) {
            for (const auto& coin : list) {
                std::string symbol = coin["coin"].asString();
                if (symbol == "USDT") continue;
                
                double size = std::stod(coin["walletBalance"].asString());
                if (size > 0) {
                    currentSymbols.push_back(symbol + "USDT");
                }
            }
        }
    }
    
    return currentSymbols;
}

bool TradingModule::shouldClosePosition(const std::string& symbol,
                                     const std::vector<std::pair<std::string, double>>& topRates) {
    // 檢查該幣對是否在新的排行中
    bool inTopRates = false;
    for (const auto& [topSymbol, rate] : topRates) {
        if (symbol == topSymbol) {
            inTopRates = true;
            break;
        }
    }
    
    // 如果不排行中，直接平
    if (!inTopRates) {
        return true;
    }
    
    // 檢查是否同時存在合約和現貨倉位
    auto positions = exchange.getPositions();
    auto spotBalances = exchange.getSpotBalances();
    
    bool hasContract = false;
    bool hasSpot = false;
    
    // 檢查合約倉位
    if (positions["result"]["list"].isArray()) {
        for (const auto& pos : positions["result"]["list"]) {
            if (pos["symbol"].asString() == symbol && 
                std::stod(pos["size"].asString()) > 0) {
                hasContract = true;
                break;
            }
        }
    }
    
    // 檢查現貨倉位
    if (spotBalances["result"]["list"].isArray()) {
        const auto& list = spotBalances["result"]["list"][0]["coin"];
        if (list.isArray()) {
            for (const auto& coin : list) {
                std::string coinSymbol = coin["coin"].asString() + "USDT";
                if (coinSymbol == symbol && 
                    std::stod(coin["walletBalance"].asString()) > 0) {
                    hasSpot = true;
                    break;
                }
            }
        }
    }
    
    // 如果不是成對倉位，需要平倉
    return !(hasContract && hasSpot);
}

// 精度輔助函數
double TradingModule::adjustContractPrecision(double quantity, const std::string& symbol) {
    double currentPrice = exchange.getContractPrice(symbol);
    
    if (currentPrice <= 0) {
        logger.error("無法獲取合約價格: " + symbol);
        return 0.0;  // 如果無法獲取價格，返回0
    }
    
    // 根據幣價動態調整精度
    if (currentPrice >= 10000.0) {  // BTC等高價幣
        return std::floor(quantity * 1000) / 1000.0;  // 0.001
    } else if (currentPrice >= 1000.0) {  // ETH等中價幣
        return std::floor(quantity * 100) / 100.0;    // 0.01
    } else if (currentPrice >= 100.0) {   // BNB等中價幣
        return std::floor(quantity * 10) / 10.0;      // 0.1
    } else {                              // 其他低價幣
        return std::floor(quantity);                 // 1.0 //只能整數
    }
}

// 精度輔助函數
double TradingModule::adjustSpotPrecision(double quantity, const std::string& symbol) {
    double currentPrice = exchange.getSpotPrice(symbol);
    
    if (currentPrice <= 0) {
        logger.error("無法獲取現貨價格: " + symbol);
        return 0.0;  // 如果無法獲取價格，返回0
    }

    
    
    // 根據幣價動態調整精度
    if (currentPrice >= 10000.0) {  // BTC等高價幣
        return std::floor(quantity * 1000) / 1000.0;  // 0.001
    } else if (currentPrice >= 1000.0) {  // ETH等中價幣
        return std::floor(quantity * 100) / 100.0;    // 0.01
    } else if (currentPrice >= 100.0) {   // BNB等中價幣
        return std::floor(quantity * 100) / 100.0;      // 0.01
    } else if (currentPrice >= 10.0) {   // 其他低價幣
        return std::floor(quantity * 100) / 100.0;      // 0.01
    } else if (currentPrice >= 1.0) {                               
        return std::floor(quantity * 100) / 100.0;      // 0.01
    } else {
        return std::floor(quantity * 10) / 10.0;                 // 0.1
    }
}

double TradingModule::getMinOrderSize(const std::string& symbol) {
    double currentPrice = exchange.getSpotPrice(symbol);
    
    // 如果法取價格，使用預設值
    if (currentPrice <= 0) {
        if (symbol == "BTCUSDT") return 0.001;
        if (symbol == "ETHUSDT") return 0.01;
        if (symbol == "BNBUSDT") return 0.1;
        if (symbol == "SOLUSDT" || 
            symbol == "AVAXUSDT" || 
            symbol == "LINKUSDT") return 0.1;
        return 1.0;
    }
    
    // 根據幣價動態調整最小訂單數量
    double minOrderValue = 5.0; // 最小訂單價值 5 USDT
    double minSize = minOrderValue / currentPrice;
    
    if (currentPrice < 1.0) {
        // 對於小於1美元的幣種，向上取整到整數
        return std::ceil(minSize);
    } else if (currentPrice < 10.0) {
        // 對於1-10美元的幣種，保留一位小數
        return std::ceil(minSize * 10) / 10.0;
    } else if (currentPrice < 100.0) {
        // 對於10-100美元的幣種保留兩位小數
        return std::ceil(minSize * 100) / 100.0;
    } else {
        // 對於高價種，保留三位小數
        return std::ceil(minSize * 1000) / 1000.0;
    }
}

void TradingModule::updateUnsupportedSymbols(const std::string& symbol) {
    Json::Value pairList;
    Json::Reader reader;
    std::ifstream file("config/pair_list.json");
    
    // 讀取現有文件內容
    if (file.is_open()) {
        if (!reader.parse(file, pairList)) {
            logger.error("解析 pair_list.json 失敗");
            return;
        }
        file.close();
    }
    
    // 保留原有的 pair_list 內容
    Json::Value originalPairList;
    if (pairList.isMember("pair_list")) {
        originalPairList = pairList["pair_list"];
    }
    
    // 確保 unsupported_symbols 陣列存在
    if (!pairList.isMember("unsupported_symbols")) {
        pairList["unsupported_symbols"] = Json::Value(Json::arrayValue);
    }
    
    // 檢查是否已經存在
    bool exists = false;
    auto& unsupportedList = pairList["unsupported_symbols"];
    for (const auto& item : unsupportedList) {
        if (item.asString() == symbol) {
            exists = true;
            break;
        }
    }
    
    // 如果不存在，則添加
    if (!exists) {
        unsupportedList.append(symbol);
        
        // 保原有的 pair_list
        pairList["pair_list"] = originalPairList;
        
        // 寫回文件
        Json::StyledWriter writer;
        std::ofstream outFile("config/pair_list.json");
        if (outFile.is_open()) {
            outFile << writer.write(pairList);
            outFile.close();
            logger.info("已將 " + symbol + " 添加到不支持的交易對列表中");
            std::cout << "已將 " + symbol + " 添加到不支持的交易對列表中" << std::endl;
        } else {
            logger.error("無法打開配置文件進行寫入: config/pair_list.json");
            std::cout << "無法打開配置文件進行寫入: config/pair_list.json" << std::endl;
        }
    } else {
        logger.info(symbol + " 已在不支持的交易對列表中");
        std::cout << symbol + " 已在不支持的交易對列表中" << std::endl;
    }
}

// 獲取當前所有倉位大小
std::map<std::string, std::pair<double, double>> TradingModule::getCurrentPositionSizes() {
    std::map<std::string, std::pair<double, double>> positionSizes;
    
    try {
        // 獲取現貨倉位
        auto spotBalances = exchange.getSpotBalances();
        if (spotBalances["result"]["list"].isArray()) {
            const auto& list = spotBalances["result"]["list"][0]["coin"];
            if (list.isArray()) {
                for (const auto& coin : list) {
                    try {
                        std::string symbol = coin["coin"].asString() + "USDT";
                        std::string balanceStr = coin["walletBalance"].asString();
                        if (!balanceStr.empty()) {
                            double size = std::stod(balanceStr);
                            if (size > 0) {
                                positionSizes[symbol].first = size;
                            }
                        }
                    } catch (const std::exception& e) {
                        logger.error("解析現貨倉位數據失敗: " + std::string(e.what()));
                        continue;
                    }
                }
            }
        }
        
        // 獲取合約倉位
        auto positions = exchange.getPositions();
        if (positions["result"]["list"].isArray()) {
            for (const auto& pos : positions["result"]["list"]) {
                try {
                    std::string symbol = pos["symbol"].asString();
                    std::string sizeStr = pos["size"].asString();
                    if (!sizeStr.empty()) {
                        double size = std::stod(sizeStr);
                        if (size > 0) {
                            positionSizes[symbol].second = size;
                        }
                    }
                } catch (const std::exception& e) {
                    logger.error("解析合約倉位數據失敗: " + std::string(e.what()));
                    continue;
                }
            }
        }
    } catch (const std::exception& e) {
        logger.error("獲取倉位信息時發生錯誤: " + std::string(e.what()));
    }

    //排除不支持的交易對
    for (const auto& symbol : Config::getInstance().getUnsupportedSymbols()) {
        positionSizes.erase(symbol);
    }
    
    return positionSizes;
}

// 處理現有倉位
void TradingModule::handleExistingPositions(
    std::map<std::string, std::pair<double, double>>& positionSizes,
    const std::vector<std::pair<std::string, double>>& topRates) {
    
    logger.info("開始處理現有倉位...");
    
    // 建立 topRates 的 symbol 集合，用於快速查找
    std::set<std::string> topSymbols;
    for (const auto& [symbol, rate] : topRates) {
        topSymbols.insert(symbol);
    }
    
    // 檢查每個現有倉位
    std::vector<std::string> positionsToClose;
    for (const auto& [symbol, sizes] : positionSizes) {
        double spotSize = sizes.first;
        double contractSize = sizes.second;
        
        // 如果該幣對不在 topRates 中，則加入關閉列表
        if (topSymbols.find(symbol) == topSymbols.end()) {
            
            std::stringstream ss;
            ss << "準備關閉 " << symbol << " 倉位 "
               << "(現貨: " << spotSize 
               << ", 合約: " << contractSize << ")";
            logger.info(ss.str());
            
            positionsToClose.push_back(symbol);
        }
    }
    
    // 關閉需要關閉的倉位
    for (const auto& symbol : positionsToClose) {
        try {
            auto it = positionSizes.find(symbol);
            if (it == positionSizes.end()) continue;
            
            double spotSize = it->second.first;
            double contractSize = it->second.second;
            
            // 關閉現貨倉位
            if (spotSize > 0) {
                spotSize = adjustSpotPrecision(spotSize, symbol);
                if (spotSize >= getMinOrderSize(symbol)) {
                    logger.info("關閉 " + symbol + " 現貨倉位: " + std::to_string(spotSize));
                    bool success = exchange.createSpotOrder(symbol, "Sell", spotSize);
                    if (!success) {
                        logger.error("關閉現貨倉位失敗: " + symbol);
                        continue;  // 如果現貨關閉失敗，不執行合約關閉
                    }
                }
            }
            
            // 關閉合約倉位
            if (contractSize > 0) {
                contractSize = adjustContractPrecision(contractSize, symbol);
                if (contractSize >= getMinOrderSize(symbol)) {
                    logger.info("關閉 " + symbol + " 合約倉位: " + std::to_string(contractSize));
                    Json::Value result = exchange.createOrder(
                        symbol, "Buy", contractSize, "linear", "MARKET");
                    if (result["retCode"].asInt() != 0) {
                        logger.error("關閉合約倉位失敗: " + symbol);
                    }
                }
            }
            
            // 等待訂單執行
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // 從 positionSizes 中移除該倉位
            positionSizes.erase(symbol);
            
        } catch (const std::exception& e) {
            logger.error("處理 " + symbol + " 倉位時發生錯誤: " + std::string(e.what()));
            continue;
        }
    }
    
    // 輸出處理結果
    if (positionsToClose.empty()) {
        logger.info("沒有需要關閉的倉位");
    } else {
        logger.info("完成關閉 " + std::to_string(positionsToClose.size()) + " 個倉位");
    }
}

// 平衡倉位
void TradingModule::balancePositions(
    const std::vector<std::pair<std::string, double>>& topRates,
    std::map<std::string, std::pair<double, double>>& positionSizes) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (topRates.empty()) {
        logger.warning("topRates 為空, 不進行倉位平衡");
        return;
    }
    
    // 獲取配置參數
    const Config& config = Config::getInstance();
    const double minPositionValue = config.getMinPositionValue();
    const double maxPositionValue = config.getMaxPositionValue();
    const auto& unsupportedSymbols = config.getUnsupportedSymbols();
    
    // 獲取賬戶狀態
    double equity = exchange.getTotalEquity();
    if (equity <= 0) {
        logger.error("無法獲取賬戶權益或權益不足");
        return;
    }
    
    // 計算當前總倉位價值
    double totalPositionValue = calculateTotalPositionValue(positionSizes, true, nullptr);
    logger.info("當前總倉位價值: " + std::to_string(totalPositionValue) + " USDT");
    
    for (const auto& [symbol, rate] : topRates) {
        try {
            logger.info("--------------------------------");
            logger.info("開始處理交易對: " + symbol);
            if (std::find(unsupportedSymbols.begin(), unsupportedSymbols.end(), symbol) 
                != unsupportedSymbols.end()) {
                logger.info("不支持的交易對: " + symbol);
                continue;
            }
            
            // 檢查現有倉位並獲取平衡結果
            auto it = positionSizes.find(symbol);
            double existingSpotSize = (it != positionSizes.end()) ? it->second.first : 0.0;
            double existingContractSize = (it != positionSizes.end()) ? it->second.second : 0.0;
            
            auto balanceCheck = checkPositionBalance(symbol, existingSpotSize, existingContractSize);
            
            // 果不需要平衡，跳過
            if (!balanceCheck.needBalance) {
                continue;
            }
            
            // 計算新的目標倉位
            double targetValue = calculatePositionSize(symbol, rate);
            if (targetValue <= 0 || targetValue < minPositionValue || 
                targetValue > maxPositionValue) {
                continue;
            }
            
            // 檢查總倉位限制
            if (totalPositionValue + targetValue > equity * config.getDefaultLeverage()) {
                logger.warning("總倉位價值將超過最大槓桿限制，跳過 " + symbol);
                continue;
            }
            
            // 執行平衡操作
            if (executeHedgePosition(symbol, targetValue, balanceCheck, positionSizes)) {
                totalPositionValue += targetValue;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            
        } catch (const std::exception& e) {
            logger.error("處理 " + symbol + " 時發生錯誤: " + std::string(e.what()));
            continue;
        }
    }
    
    logger.info("倉位平衡完成，最新總倉位價值: " + 
                std::to_string(totalPositionValue) + " USDT (" + 
                std::to_string(totalPositionValue / equity * 100) + "% 槓桿率)");
}

bool TradingModule::executeHedgePosition(
    const std::string& symbol,
    double targetValue,
    const BalanceCheckResult& balanceCheck,
    std::map<std::string, std::pair<double, double>>& positionSizes) {
    
    logger.info("開始執行對衝交易平衡: " + symbol); 
    try {
        // 1. 檢查是否需要平衡
        if (!balanceCheck.needBalance) {
            logger.info(symbol + " 無需平衡倉位");
            return true;
        }

        // 2. 獲取當前價格
        double currentPrice = exchange.getSpotPrice(symbol);
        if (currentPrice <= 0) {
            logger.error("無法獲取 " + symbol + " 價格");
            return false;
        }
        
        // 3. 計算目標數量並調整精度
        double targetQuantity = adjustSpotPrecision(targetValue / currentPrice, symbol);
        
        // 4. 檢查最小訂單要求
        if (targetQuantity < getMinOrderSize(symbol)) {
            logger.info(symbol + " 數量小於最小訂單要求");
            return false;
        }
        
        // 5. 關閉現有倉位
        auto it = positionSizes.find(symbol);
        if (it != positionSizes.end()) {
            if (it->second.first > 0) {
                double spotCloseQty = adjustSpotPrecision(it->second.first, symbol);
                bool spotCloseSuccess = exchange.createSpotOrder(
                    symbol, "Sell", spotCloseQty);
                if (!spotCloseSuccess) {
                    logger.error("關閉現有現貨倉位失敗: " + symbol);
                    return false;
                }
            }
            
            if (it->second.second > 0) {
                double contractCloseQty = adjustContractPrecision(it->second.second, symbol);
                Json::Value result = exchange.createOrder(
                    symbol, "Buy", contractCloseQty, "linear", "MARKET");
                if (result["retCode"].asInt() != 0) {
                    logger.error("關閉現有合約倉位失敗: " + symbol);
                    return false;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // 6. 建立新倉位
        bool spotSuccess = createSpotOrderIncludeFee(symbol, "Buy", targetQuantity);
        if (!spotSuccess) {
            logger.error("建立現貨倉位失敗: " + symbol);
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        double contractQty = adjustContractPrecision(targetQuantity, symbol);
        Json::Value result = exchange.createOrder(
            symbol, "Sell", contractQty, "linear", "MARKET"
        );
        
        if (result["retCode"].asInt() != 0) {
            logger.error("建立合約倉位失敗: " + symbol);
            exchange.createSpotOrder(symbol, "Sell", targetQuantity);
            return false;
        }
        
        // 7. 更新倉位記錄
        positionSizes[symbol] = std::make_pair(targetQuantity, contractQty);
        
        logger.info(symbol + " 對衝交易完成: " +
                   "現貨=" + std::to_string(targetQuantity) +
                   ", 合約=" + std::to_string(contractQty) +
                   ", 價差=" + std::to_string(balanceCheck.priceDiff * 100) + "%" +
                   ", 預期收益=" + std::to_string(balanceCheck.expectedProfit) + " USDT");
                   
        return true;
        
    } catch (const std::exception& e) {
        logger.error("執行對衝交易時發生錯誤: " + std::string(e.what()));
        return false;
    }
}

// 錯誤處理
void TradingModule::handleError(const std::string& symbol, const std::string& error) {
    logger.error("交易錯誤: " + error);
    if (error.find("Not supported symbols") != std::string::npos) {
        updateUnsupportedSymbols(symbol);
    }
}

TradingModule::BalanceCheckResult TradingModule::checkPositionBalance(const std::string& symbol, 
                                      double spotSize, 
                                      double contractSize) {
    BalanceCheckResult result{false, 0.0, 0.0, 0.0, 0.0};
    logger.info("開始檢查對衝合約現貨組合倉位平衡: " + symbol);
    
    // 獲取配置參數
    const Config& config = Config::getInstance();
    double minPositionValue = config.getMinPositionValue();
    double maxPositionValue = config.getMaxPositionValue();
    const bool isSpotMarginTradingEnabled = config.isSpotMarginTradingEnabled();
    
    // 1. 檢查倉位數量是否對等
    logger.info("現貨倉位: " + std::to_string(spotSize));
    logger.info("合約倉位: " + std::to_string(contractSize));
    double sizeDiff = std::abs(spotSize - contractSize);
    const double SIZE_DIFF_THRESHOLD = 0.003;  // 0.3% 誤差容忍度
    bool sizeBalanced = (sizeDiff <= std::min(spotSize, contractSize) * SIZE_DIFF_THRESHOLD);
    
    // 2. 獲取價格資訊
    double spotPrice = exchange.getSpotPrice(symbol);
    double contractPrice = exchange.getContractPrice(symbol);
    if (spotPrice <= 0 || contractPrice <= 0) {
        logger.error("無法獲取 " + symbol + " 價格信息");
        return result;
    }
    
    // 3. 計算倉位價值
    double spotValue = spotSize * spotPrice;
    double contractValue = contractSize * contractPrice;
    
    // 根據是否支援現貨保證金來計算倉位總價值
    double pairValue;
    if (isSpotMarginTradingEnabled) {
        pairValue = (spotValue + contractValue) / 2;
        logger.info("支援現貨保證金，使用平均倉位價值計算");
    } else {
        pairValue = spotValue + contractValue;
        logger.info("不支援現貨保證金，使用對衝組合總倉位價值計算");
    }
    
    logger.info("倉位價值計算:");
    logger.info("- 現貨價值: " + std::to_string(spotValue) + " USDT");
    logger.info("- 合約價值: " + std::to_string(contractValue) + " USDT");
    logger.info("- 對衝組合總倉位價值: " + std::to_string(pairValue) + " USDT");
    
    // 4. 檢查倉位價值是否在允許範圍內
    bool valueInRange = (pairValue >= minPositionValue && pairValue <= maxPositionValue);

    // 5. 計算預測倉位大小 (使用最小倉位價值)
    double predictedSpotSize = minPositionValue / spotPrice;
    double predictedContractSize = minPositionValue / contractPrice;
    
    // 6. 計算價格差
    result.priceDiff = std::abs(spotPrice - contractPrice) / spotPrice;
    
    // 7. 分別計算現貨和合約的深度影響
    auto spotOrderbook = exchange.getSpotOrderBook(symbol);
    auto contractOrderbook = exchange.getContractOrderBook(symbol);
    
    double spotDepthImpact = calculateDepthImpact(spotOrderbook, predictedSpotSize);
    double contractDepthImpact = calculateDepthImpact(contractOrderbook, predictedContractSize);
    
    // 使用較大的深度影響作為最終結果
    result.depthImpact = std::max(spotDepthImpact, contractDepthImpact);
    
    // 8. 計算預估成本和預期收益
    double spotCost = calculateRebalanceCost(symbol, predictedSpotSize, true, spotOrderbook );  // true 表示現貨
    double contractCost = calculateRebalanceCost(symbol, predictedContractSize, false, contractOrderbook); // false 表示合約
    result.estimatedCost = spotCost + contractCost;
    
    double fundingRate = exchange.getCurrentFundingRate(symbol);
    // 使用較小的倉位大小計算預期收益（保守估計）
    double minSize = std::min(predictedSpotSize, predictedContractSize);
    result.expectedProfit = calculateExpectedProfit(minSize, fundingRate);
    
    // 9. 判斷是否需要重平衡
    const double PRICE_DIFF_THRESHOLD = 0.001;    // 0.1% 價格差異閾值
    const double DEPTH_IMPACT_THRESHOLD = 0.0005; // 0.05% 深度影響閾值
    const double MIN_PROFIT_RATIO = 1.5;         // 最小收益/成本比
    
    result.needBalance = 
        (!sizeBalanced || !valueInRange) &&
        (result.priceDiff < PRICE_DIFF_THRESHOLD) &&
        (result.depthImpact < DEPTH_IMPACT_THRESHOLD) &&
        (result.expectedProfit > result.estimatedCost * MIN_PROFIT_RATIO);
    
    // 10. 記錄詳細日誌
    logger.info(symbol + " 倉位檢查結果:");
    logger.info("- 現貨倉位: " + std::to_string(spotSize) + 
               " (" + std::to_string(spotValue) + " USDT)");
    logger.info("- 合約倉位: " + std::to_string(contractSize) + 
               " (" + std::to_string(contractValue) + " USDT)");
    logger.info("- 倉位數量對等: " + std::string(sizeBalanced ? "是" : "否"));
    logger.info("- 倉位價值在範圍內: " + std::string(valueInRange ? "是" : "否"));
    logger.info("- 預測現貨倉位: " + std::to_string(predictedSpotSize));
    logger.info("- 預測合約倉位: " + std::to_string(predictedContractSize));
    logger.info("- 價格差異: " + std::to_string(result.priceDiff * 100) + "%");
    logger.info("- 現貨深度影響: " + std::to_string(spotDepthImpact * 100) + "%");
    logger.info("- 合約深度影響: " + std::to_string(contractDepthImpact * 100) + "%");
    logger.info("- 最終深度影響: " + std::to_string(result.depthImpact * 100) + "%");
    logger.info("- 現貨預估成本: " + std::to_string(spotCost) + " USDT");
    logger.info("- 合約預估成本: " + std::to_string(contractCost) + " USDT");
    logger.info("- 總預估成本: " + std::to_string(result.estimatedCost) + " USDT");
    logger.info("- 預期收益: " + std::to_string(result.expectedProfit) + " USDT");
    logger.info("- 需要重平衡: " + std::string(result.needBalance ? "是" : "否"));
    
    return result;
}

// 計算深度影響
double TradingModule::calculateDepthImpact(const Json::Value& orderbook, double size) {
    try {
        // 檢查訂單簿數據格式
        if (!orderbook.isObject() || !orderbook["result"].isObject() || 
            !orderbook["result"]["a"].isArray() || orderbook["result"]["a"].empty()) {
            logger.error("訂單簿數據格式無效");
            return 0.0;
        }

        double totalImpact = 0.0;
        double remainingSize = std::abs(size);  // 使用絕對值
        const Json::Value& asks = orderbook["result"]["a"];
        
        // 獲取基準價格（第一個賣單價格）
        if (!asks[0].isArray() || asks[0].size() < 2) {
            logger.error("訂單簿價格數據格式無效");
            return 0.0;
        }
        
        double basePrice = std::stod(asks[0][0].asString());
        
        // 遍歷賣單計算滑點
        for (const auto& level : asks) {
            if (!level.isArray() || level.size() < 2) {
                continue;
            }
            
            try {
                double price = std::stod(level[0].asString());
                double quantity = std::stod(level[1].asString());
                
                double levelImpact = 0.0;
                if (remainingSize <= quantity) {
                    levelImpact = remainingSize * (price - basePrice) / basePrice;
                    totalImpact += levelImpact;
                    break;
                } else {
                    levelImpact = quantity * (price - basePrice) / basePrice;
                    totalImpact += levelImpact;
                    remainingSize -= quantity;
                }
                
                logger.debug("深度級別: 價格=" + std::to_string(price) + 
                           ", 數量=" + std::to_string(quantity) + 
                           ", 影響=" + std::to_string(levelImpact));
                
            } catch (const std::exception& e) {
                logger.error("處理深度數據時發生錯誤: " + std::string(e.what()));
                continue;
            }
        }
        
        // 如果還有剩餘未匹配的數量，記錄警告
        if (remainingSize > 0) {
            logger.warning("深度不足以完全匹配訂單大小，剩餘: " + std::to_string(remainingSize));
        }
        
        return size != 0 ? totalImpact / std::abs(size) : 0.0;
        
    } catch (const std::exception& e) {
        logger.error("計算深度影響時發生錯誤: " + std::string(e.what()));
        return 0.0;
    }
}

// 計算平衡成本
double TradingModule::calculateRebalanceCost(const std::string& symbol, double size, bool isSpot, const Json::Value& orderbook) {
    try {
        
        // 檢查訂單簿數據是否有效
        if (!orderbook.isObject() || !orderbook["result"].isObject() || 
            !orderbook["result"]["a"].isArray() || orderbook["result"]["a"].empty()) {
            logger.error(std::string(isSpot ? "現貨" : "合約") + "訂單簿數據無效");
            return 0.0;
        }

        // 獲取對應的手續費率
        double feeRate = isSpot ? exchange.getSpotFeeRate() : exchange.getContractFeeRate();
        
        // 計算滑點成本
        double remainingSize = size;
        double totalCost = 0.0;
        const Json::Value& asks = orderbook["result"]["a"];
        double basePrice = std::stod(asks[0][0].asString());  // 最佳賣價作為基準價格
        
        for (const auto& level : asks) {
            if (!level.isArray() || level.size() < 2) continue;
            
            double price = std::stod(level[0].asString());
            double quantity = std::stod(level[1].asString());
            
            if (remainingSize <= quantity) {
                // 最後一筆訂單
                totalCost += remainingSize * (price - basePrice);
                break;
            } else {
                // 完全吃掉這一層深度
                totalCost += quantity * (price - basePrice);
                remainingSize -= quantity;
            }
        }
        
        // 計算手續費成本
        double tradingFee = size * basePrice * feeRate;
        
        // 總成本 = 滑點成本 + 手續費成本
        double totalRebalanceCost = totalCost + tradingFee;
        
        // 記錄詳細成本信息
        logger.info(std::string(isSpot ? "現貨" : "合約") + "重平衡成本計算:");
        logger.info("- 基準價格: " + std::to_string(basePrice) + " USDT");
        logger.info("- 交易數量: " + std::to_string(size));
        logger.info("- 滑點成本: " + std::to_string(totalCost) + " USDT");
        logger.info("- 手續費率: " + std::to_string(feeRate * 100) + "%");
        logger.info("- 手續費成本: " + std::to_string(tradingFee) + " USDT");
        logger.info("- 總成本: " + std::to_string(totalRebalanceCost) + " USDT");
        
        return totalRebalanceCost;
        
    } catch (const std::exception& e) {
        logger.error("計算重平衡成本時發生錯誤: " + std::string(e.what()));
        return 0.0;
    }
}

// 計算預期收益
double TradingModule::calculateExpectedProfit(double size, double fundingRate) {
    //資金費率年化率 = 資金費率(%) * 365 * 3
    double annualRate = fundingRate * 3 * 365;
    
    // 從設定檔獲取預期持有天數
    const int holdingDays = Config::getInstance().getFundingHoldingDays();
    
    // 年化轉換為持有收益
    double periodRate = annualRate * (holdingDays / 365.0);
    
    return size * std::abs(periodRate);
}


bool TradingModule::createSpotOrderIncludeFee(const std::string& symbol, const std::string& side, double qty) {
    double fee = exchange.getSpotFeeRate();
    qty = qty * (1 + fee * ( 1 + fee )); //現貨倉位
    qty = adjustSpotPrecision(qty, symbol);
    logger.info("實際現貨含手續費下單倉位: " + std::to_string(qty) + " " + symbol);
    return exchange.createSpotOrder(symbol, side, qty);
}


// 計算總倉位價值
double TradingModule::calculateTotalPositionValue(
    const std::map<std::string, std::pair<double, double>>& positions,
    bool positionsIsSize = false,
    const std::map<std::string, std::pair<double, double>>* prices = nullptr) {
    
    double totalValue = 0.0;
    const bool isSpotMarginTradingEnabled = Config::getInstance().isSpotMarginTradingEnabled();
    
    for (const auto& [symbol, position] : positions) {
        double spotValue = 0.0;
        double contractValue = 0.0;
        
        // 根據不同情況計算倉位價值
        if (positionsIsSize && prices != nullptr) {
            // 使用提供的價格
            auto priceIt = prices->find(symbol);
            if (priceIt != prices->end()) {
                double spotPrice = priceIt->second.first;
                double contractPrice = priceIt->second.second;
                
                if (spotPrice > 0) {
                    spotValue = position.first * spotPrice;
                    contractValue = position.second * contractPrice;
                    
                    logger.debug(symbol + " 倉位計算: " 
                        + "現貨=" + std::to_string(spotValue) + " USDT"
                        + " (" + std::to_string(position.first) + "*" + std::to_string(spotPrice) + ")"
                        + ", 合約=" + std::to_string(contractValue) + " USDT"
                        + " (" + std::to_string(position.second) + "*" + std::to_string(contractPrice) + ")");
                }
            }
        } else if (!positionsIsSize && prices == nullptr) {
            // position 已經包含價值
            spotValue = position.first;
            contractValue = position.second;
        } else {
            // 需要從交易所獲取即時價格
            double spotPrice = exchange.getSpotPrice(symbol);
            double contractPrice = exchange.getContractPrice(symbol);
            
            if (spotPrice > 0) {
                spotValue = position.first * spotPrice;
                contractValue = position.second * contractPrice;
                
                logger.debug(symbol + " 倉位計算: " 
                    + "現貨=" + std::to_string(spotValue) + " USDT"
                    + " (" + std::to_string(position.first) + "*" + std::to_string(spotPrice) + ")"
                    + ", 合約=" + std::to_string(contractValue) + " USDT"
                    + " (" + std::to_string(position.second) + "*" + std::to_string(contractPrice) + ")");
            }
        }
        
        // 根據模式計算總價值
        if (isSpotMarginTradingEnabled) {
            totalValue += (spotValue + contractValue) / 2;
        } else {
            totalValue += spotValue + contractValue;
        }
    }
    
    logger.info("總倉位價值: " + std::to_string(totalValue) + " USDT" + 
                (isSpotMarginTradingEnabled ? " (使用平均值計算)" : " (使用總和計算)"));
    
    return totalValue;
}

void TradingModule::displayPositions() {
    Logger logger;
    const bool isSpotMarginTradingEnabled = Config::getInstance().isSpotMarginTradingEnabled();
    
    // 獲取資金費率排行
    auto topRates = getTopFundingRates();
    std::set<std::string> topSymbols;
    for (const auto& [symbol, rate] : topRates) {
        topSymbols.insert(symbol);
    }
    
    // 獲取合約倉位
    Json::Value futuresPositions = exchange.getPositions();
    
    // 獲取現貨餘額
    Json::Value spotBalances = exchange.getSpotBalances();
    
    // 顯示標題
    std::cout << "\n=== 當前持倉狀態 ===" << std::endl;
    if (isSpotMarginTradingEnabled) {
        std::cout << "(使用現貨保證金模式，倉位價值以平均值計算)" << std::endl;
    } else {
        std::cout << "(使用標準模式，倉位價值以總和計算)" << std::endl;
    }
    
    // 顯示表頭
    std::cout << std::left
              << std::setw(15) << "幣對"
              << std::setw(12) << "類型"
              << std::setw(12) << "方向"
              << std::setw(18) << "數量"
              << std::setw(15) << "價格"
              << std::setw(15) << "資金費率"
              << std::setw(18) << "未實現盈虧"
              << std::setw(15) << "倉位價值"
              << std::endl;
    std::cout << std::string(120, '-') << std::endl;
    
    double totalValue = 0.0;
    double totalPnL = 0.0;
    std::map<std::string, std::pair<double, double>> symbolValues;
    
    // 顯示合約倉位
    if (futuresPositions.isObject() && futuresPositions["result"]["list"].isArray()) {
        for (const auto& pos : futuresPositions["result"]["list"]) {
            try {
                std::string symbol = pos["symbol"].asString();
                if (topSymbols.find(symbol) == topSymbols.end()) continue;
                
                double size = std::stod(pos["size"].asString());
                if (size <= 0) continue;
                
                double positionValue = std::stod(pos["positionValue"].asString());
                double price = std::stod(pos["avgPrice"].asString());
                double fundingRate = exchange.getCurrentFundingRate(symbol);
                symbolValues[symbol].second = positionValue;
                
                std::cout << std::left
                          << std::setw(15) << symbol
                          << std::setw(12) << "合約"
                          << std::setw(12) << pos["side"].asString()
                          << std::setw(18) << std::fixed << std::setprecision(4) << size
                          << std::setw(15) << std::fixed << std::setprecision(4) << price
                          << std::setw(15) << std::fixed << std::setprecision(4) << fundingRate * 100 << "%"
                          << std::setw(18) << std::fixed << std::setprecision(2) 
                          << std::stod(pos["unrealisedPnl"].asString())
                          << std::setw(15) << std::fixed << std::setprecision(2) 
                          << positionValue
                          << std::endl;
                
                totalPnL += std::stod(pos["unrealisedPnl"].asString());
            } catch (const std::exception& e) {
                logger.error("解析合約倉位數據失敗: " + std::string(e.what()));
                continue;
            }
        }
    }
    
    // 顯示現貨餘額
    if (spotBalances.isObject() && spotBalances["result"]["list"].isArray()) {
        const auto& list = spotBalances["result"]["list"][0]["coin"];
        if (list.isArray()) {
            for (const auto& coin : list) {
                try {
                    std::string symbol = coin["coin"].asString();
                    if (symbol == "USDT") continue;
                    
                    std::string pairSymbol = symbol + "USDT";
                    if (topSymbols.find(pairSymbol) == topSymbols.end()) continue;
                    
                    double size = std::stod(coin["walletBalance"].asString());
                    if (size <= 0) continue;
                    
                    double spotPrice = exchange.getSpotPrice(pairSymbol);
                    double positionValue = size * spotPrice;
                    double fundingRate = exchange.getCurrentFundingRate(pairSymbol);
                    symbolValues[pairSymbol].first = positionValue;
                    
                    if (positionValue > 0) {
                        std::cout << std::left
                                  << std::setw(15) << pairSymbol
                                  << std::setw(12) << "現貨"
                                  << std::setw(12) << "Buy"
                                  << std::setw(18) << std::fixed << std::setprecision(4) << size
                                  << std::setw(15) << std::fixed << std::setprecision(4) << spotPrice
                                  << std::setw(15) << std::fixed << std::setprecision(4) << fundingRate * 100 << "%"
                                  << std::setw(18) << std::fixed << std::setprecision(2) << 0.0
                                  << std::setw(15) << std::fixed << std::setprecision(2) << positionValue
                                  << std::endl;
                    }
                } catch (const std::exception& e) {
                    logger.error("解析現貨餘額數據失敗: " + std::string(e.what()));
                    continue;
                }
            }
        }
    }
    
    // 計算總倉位價值
    totalValue = calculateTotalPositionValue(symbolValues, false, nullptr);
    
    // 顯示匯總信息
    std::cout << std::string(120, '-') << std::endl;
    std::cout << "總倉位價值: " << std::fixed << std::setprecision(2) << totalValue << " USDT" 
              << (isSpotMarginTradingEnabled ? " (平均值)" : " (總和)") << std::endl;
    std::cout << "總未實現盈虧: " << std::fixed << std::setprecision(2) << totalPnL << " USDT" << std::endl;
    
    double equity = exchange.getTotalEquity();
    if (equity > 0) {
        std::cout << "賬戶總權益: " << std::fixed << std::setprecision(2) << equity << " USDT" << std::endl;
        double utilizationRate = (totalValue / equity) * 100;
        std::cout << "倉位使用率: " << std::fixed << std::setprecision(2) << utilizationRate << "%" << std::endl;
    }
}

std::vector<std::string> TradingModule::getSymbolsByCMC(int topCount) {
    Logger logger;
    std::vector<std::string> selectedSymbols;
    const Config& config = Config::getInstance();
    
    
    try {
        CURL* curl = curl_easy_init();
        if (!curl) {
            logger.error("無法初始化 CURL");
            return {};
        }
        const std::string& sortBy = config.getCMCSortBy();
        logger.info("開始從 CMC 獲取交易對列表 " + std::to_string(topCount) + " 個，排序方式: " + sortBy);

        
        // 設置 API 請求
        std::string apiKey = config.getCMCApiKey();
        std::string url = "https://pro-api.coinmarketcap.com/v1/cryptocurrency/listings/latest?sort=" + sortBy + "&limit=" 
        + std::to_string(topCount) + "&aux=" + sortBy;
        logger.debug("請求 URL: " + url);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-CMC_PRO_API_KEY: " + apiKey).c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            logger.error("CMC API 請求失敗: " + std::string(curl_easy_strerror(res)));
            return {};
        }
        
        logger.debug("收到 CMC 響應，長度: " + std::to_string(response.length()));
        // logger.debug("收到 CMC 響應: " + response);

        
        // 解析 JSON 響應
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(response, root)) {
            logger.error("解析 CMC 響應失敗");
            logger.debug("響應內容: " + response);
            return {};
        }
        
        // 檢查錯誤狀態
        if (root.isMember("status")) {
            logger.debug("API 狀態: " + root["status"]["error_message"].asString());
        }
        
        // 處理數據
        if (root["data"].isArray()) {
            
            for (const auto& coin : root["data"]) {
                if (selectedSymbols.size() >= static_cast<size_t>(topCount)) {
                    logger.debug("已達到設定的 Top " + std::to_string(topCount) + " 數量，停止處理");
                    break;
                }
                
                std::string symbol = coin["symbol"].asString() + "USDT";
                // double sortByValue = coin["quote"]["USD"][sortBy].asDouble();
                // logger.debug(symbol + " - " + sortBy + ": " + std::to_string(sortByValue));
                selectedSymbols.push_back(symbol);
            }
            
            // 顯示選中的交易對
            logger.info("根據 " + sortBy + " 排序選擇了 " + 
                       std::to_string(selectedSymbols.size()) + " 個交易對:");
            for (const auto& symbol : selectedSymbols) {
                logger.info("- " + symbol);
            }
        } else {
            logger.error("CMC 響應中沒有找到數據數組");
            logger.debug("響應內容: " + response);
        }
        
    } catch (const std::exception& e) {
        logger.error("獲取 CMC 數據時發生錯誤: " + std::string(e.what()));
    }
    
    return selectedSymbols;
}

size_t TradingModule::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}


void TradingModule::displayPositionSizes(
    const std::map<std::string, std::pair<double, double>>& positionSizes) {
    
    if (!positionSizes.empty()) {
        std::cout << "\n=== 保留的倉位 ===" << std::endl;
        for (const auto& [symbol, sizes] : positionSizes) {
            double spotSize = sizes.first;
            double contractSize = sizes.second;
            // 格式化輸出
            std::cout << std::left 
                     << std::setw(12) << symbol 
                     << "現貨: " << std::setw(10) << std::fixed << std::setprecision(4) << spotSize
                     << "合約: " << std::setw(10) << std::fixed << std::setprecision(4) << contractSize
                     << std::endl;
        }
        std::cout << std::endl;
    } else {
        logger.info("所有倉位已清空");
    }
}