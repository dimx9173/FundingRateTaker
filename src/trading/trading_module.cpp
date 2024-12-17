#include "trading/trading_module.h"
#include "config.h"
#include <iostream>
#include <ctime>
#include <iomanip>
#include <thread>
#include <chrono>
#include <set>
#include <fstream>

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
        adjustedPosition = basePosition * (1 + std::abs(rate) * scalingFactor);
        // 限制最大倉位價值
        adjustedPosition = std::min(adjustedPosition, maxPositionValue);
    }

    // 計算數量並調整精度
    double quantity = adjustedPosition / currentPrice;
    quantity = adjustSpotPrecision(quantity, symbol);
    
    // 確保最終倉位價值不小於最小要求
    double finalValue = quantity * currentPrice;
    if (finalValue < minPositionValue) {
        return 0.0;  // 放棄該交易
    }

    return finalValue;
}

std::vector<std::pair<std::string, double>> TradingModule::getTopFundingRates() {
    if (isNearSettlement()) {
        logger.info("接近結算時間，重新計算資金費率...");
    }
    
    // 將 vector 轉換為 set 以便快速查找
    auto unsupportedList = Config::getInstance().getUnsupportedSymbols();
    std::set<std::string> unsupportedSymbols(unsupportedList.begin(), unsupportedList.end());
    
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
        historicalRates = exchange.getFundingHistory();
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
    int topCount = Config::getInstance().getTopPairsCount();
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
    
    return weightedRates;
}

void TradingModule::closeTradeGroup(const std::string& group) {
    auto parts = splitString(group, ":");
    if (parts.size() >= 2) {
        std::string symbol = parts[1];
        exchange.closePosition(symbol);
    }
}

void TradingModule::executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates) {
    logger.info("開始執行對衝策略...");
    
    // 1. 檢查是否接近結算時間
    if (isNearSettlement()) {
        logger.info("接近結算時間，暫停交易");
        // return;
    }
    
    try {
        // 2. 獲取當前倉位狀態
        auto positionSizes = getCurrentPositionSizes();
        if (positionSizes.empty()) {
            logger.info("無法獲取倉位信息，跳過本次執行");
            return;
        }
        
        // 3. 處理現有倉位的平倉
        auto currentSymbols = getCurrentPositionSymbols();
        if (!currentSymbols.empty()) {
            logger.info("開始處理現有倉位...");
            handleExistingPositions(currentSymbols, topRates);
        }
        
        // // 4. 處理新建倉位
        // if (!topRates.empty()) {
        //     logger.info("開始處理新建倉位...");
        //     handleNewPositions(topRates, positionSizes);
        // }
        
        // 5. 新建倉位並平衡現有倉位
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
    for (const auto& time_str : settlement_times) {
        std::istringstream ss(time_str);
        int hour, minute;
        char delimiter;
        ss >> hour >> delimiter >> minute;
        int settlement_minutes = hour * 60 + minute;
        int diff = std::abs(current_minutes - settlement_minutes);
        
        // 考慮跨日情況
        if (diff > 720) { // 12時 = 720分鐘
            diff = 1440 - diff; // 24小時 = 1440分鐘
        }
        
        if (diff <= pre_minutes) {
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
    
    return positionSizes;
}

// 處理新建倉位
void TradingModule::handleNewPositions(
    const std::vector<std::pair<std::string, double>>& topRates,
    const std::map<std::string, std::pair<double, double>>& positionSizes) {
    
    for (const auto& [symbol, rate] : topRates) {
        // 檢查是否已有倉位
        if (positionSizes.find(symbol) != positionSizes.end()) {
            logger.info("已有倉位: " + symbol);
            continue;
        }
        
        // 計算倉位大小
        double positionSize = calculatePositionSize(symbol, rate);
        if (positionSize <= 0) continue;
        
        // 獲取當前價格
        double spotPrice = exchange.getSpotPrice(symbol);
        if (spotPrice <= 0) continue;
        
        // 計算數量
        double quantity = positionSize / spotPrice;
        quantity = adjustSpotPrecision(quantity, symbol);
        
        // 檢查最小訂單量
        if (quantity < getMinOrderSize(symbol)) {
            logger.info("數量小於最小訂單量: " + symbol);
            continue;
        }
        
        logger.info("開始建立新對衝倉位: " + symbol);
        logger.info("倉位大小: " + std::to_string(positionSize) + " USDT");
        logger.info("數量: " + std::to_string(quantity));
        
        // 建立現貨多倉
        bool spotSuccess = createSpotOrderIncludeFee(symbol, "Buy", quantity);
        if (!spotSuccess) {
            handleError(symbol, exchange.getLastError());
            continue;
        }
        
        // 等待現貨訂單完成
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 建立合約空倉
        Json::Value result = exchange.createOrder(symbol, "Sell", quantity, "linear", "MARKET");
        if (result["retCode"].asInt() != 0) {
            handleError(symbol, result["retMsg"].asString());
            // 回滾現貨交易
            exchange.createSpotOrder(symbol, "Sell", quantity);
            continue;
        }
        
        logger.info("成功建立對衝倉位: " + symbol);
    }
}

// 處理現有倉位
void TradingModule::handleExistingPositions(
    const std::vector<std::string>& currentSymbols,
    const std::vector<std::pair<std::string, double>>& topRates) {
    
    for (const auto& symbol : currentSymbols) {
        if (shouldClosePosition(symbol, topRates)) {
            logger.info("準備關閉倉位: " + symbol);
            
            // 獲取現貨和合約倉位數量
            auto positionSizes = getCurrentPositionSizes();
            auto it = positionSizes.find(symbol);
            if (it == positionSizes.end()) continue;
            
            double spotSize = it->second.first;
            double contractSize = it->second.second;
            
            // 關閉現貨倉位
            if (spotSize > 0) {
                spotSize = adjustSpotPrecision(spotSize, symbol);
                if (spotSize >= getMinOrderSize(symbol)) {
                    bool success = exchange.createSpotOrder(symbol, "Sell", spotSize);
                    if (!success) {
                        logger.error("關閉現貨倉位失敗: " + symbol);
                    }
                }
            }
            
            // 關閉合約倉位
            if (contractSize > 0) {
                contractSize = adjustContractPrecision(contractSize, symbol);
                if (contractSize >= getMinOrderSize(symbol)) {
                    Json::Value result = exchange.createOrder(symbol, "Buy", contractSize, "linear", "MARKET");
                    if (result["retCode"].asInt() != 0) {
                        logger.error("關閉合約倉位失敗: " + symbol);
                    }
                }
            }
        }
    }
}

// 平衡倉位
void TradingModule::balancePositions(
    const std::vector<std::pair<std::string, double>>& topRates,
    std::map<std::string, std::pair<double, double>>& positionSizes) {
    
    for (const auto& [symbol, rate] : topRates) {
        logger.info("------開始平衡對衝合約現貨組合: " + symbol);
        auto it = positionSizes.find(symbol);
        if (it == positionSizes.end()) continue;
        
        double spotSize = it->second.first;
        double contractSize = it->second.second;
        
        // 檢查對衝合約現貨組合平衡資訊
        auto balanceCheck = checkPositionBalance(symbol, spotSize, contractSize);
        
        if (!balanceCheck.needBalance) {
            logger.info(symbol + " 無需重平衡: " +
                       "價差=" + std::to_string(balanceCheck.priceDiff * 100) + "%, " +
                       "深度影響=" + std::to_string(balanceCheck.depthImpact * 100) + "%, " +
                       "預期成本=" + std::to_string(balanceCheck.estimatedCost) + " USDT, " +
                       "預期收益=" + std::to_string(balanceCheck.expectedProfit) + " USDT");
            continue;
        }
        logger.info("------需要重平衡: " + symbol);

        // 執行重平衡邏輯
        //1.確認現貨倉位價值是否介於 min_position_value/2 和 max_position_value/2 之間
        //不足需先買入，超過需先賣出
        const Config& config = Config::getInstance();
        double maxPositionValue = config.getMaxPositionValue();
        double minPositionValue = config.getMinPositionValue();
        spotSize = exchange.getSpotBalance(symbol);

        double spotPrice = exchange.getSpotPrice(symbol);
        double spotValue = spotSize * spotPrice;
        if (spotValue < minPositionValue / 2 || spotValue > maxPositionValue / 2) {
            logger.info("現貨倉位價值: " + std::to_string(spotValue) + " USDT, 需要重平衡");
            if (spotValue < minPositionValue / 2) {
                double needBuySize = adjustSpotPrecision((minPositionValue / 2 - spotValue) / spotPrice, symbol);
                logger.info("現貨倉位價值不足,需補足: " + std::to_string(needBuySize) + " " + symbol);
                bool sizeDiffValid = needBuySize >= getMinOrderSize(symbol);//倉位差異是否大於最小訂單量
                if (sizeDiffValid) {
                    bool spotSuccess = createSpotOrderIncludeFee(symbol, "Buy", needBuySize);
                    if (!spotSuccess) {
                        logger.error("重平衡現貨倉位失敗: " + symbol);
                        continue;
                    }
                } else {
                    logger.info("現貨倉位價值不足,但訂單量不足,不用補足");
                }
            } else if (spotValue > maxPositionValue / 2) {
                double needSellSize = adjustSpotPrecision((spotValue - maxPositionValue / 2) / spotPrice, symbol);
                logger.info("現貨倉位價值超過,需賣出: " + std::to_string(needSellSize) + " " + symbol);
                bool sizeDiffValid = needSellSize >= getMinOrderSize(symbol);//倉位差異是否大於最小訂單量
                if (sizeDiffValid) {
                    bool spotSuccess = exchange.createSpotOrder(symbol, "Sell", needSellSize);
                    if (!spotSuccess) {
                        logger.error("重平衡現貨倉位失敗: " + symbol);
                        continue;
                    }
                } else {
                    logger.info("現貨倉位價值超過,但訂單量不足,不用賣出");
                }
            }
        }
        //現貨倉位更新
        spotSize = exchange.getSpotBalance(symbol);
        //2.確認對衝合約現貨組合倉位差異是否平衡，不足需平衡
        double sizeDiff = std::abs(spotSize - contractSize);//倉位差異
        bool sizeDiffValid = sizeDiff >= getMinOrderSize(symbol);//倉位差異是否大於最小訂單量
        sizeDiff = adjustContractPrecision(sizeDiff, symbol);//調整精度
        logger.info("倉位差異: " + std::to_string(sizeDiff) + " " + symbol);
        if (sizeDiffValid) {
            // 由於現貨倉位已經平衡，所以只需要平衡合約倉位
            // 判斷哪個需要增加倉位
            if (spotSize < contractSize) {
                // 現貨倉位較小,需要減少合約
                logger.info("減少合約空倉: " + std::to_string(sizeDiff) + " " + symbol);
                Json::Value result = exchange.createOrder(
                    symbol, "Buy", sizeDiff, "linear", "MARKET"
                );
                if (result["retCode"].asInt() != 0) {
                    logger.error("重平衡合約倉位失敗: " + symbol);
                    continue;
                }
            } else {
                // 現貨倉位較大,需要增加合約
                logger.info("增加合約空倉: " + std::to_string(sizeDiff) + " " + symbol);
                Json::Value result = exchange.createOrder(
                    symbol, "Sell", sizeDiff, "linear", "MARKET"
                );
                if (result["retCode"].asInt() != 0) {
                    logger.error("重平衡合約倉位失敗: " + symbol);
                    continue;
                }
            }
            
            // 等待訂單執行
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            // 驗證重平衡結果
            auto newPositions = getCurrentPositionSizes();
            auto newIt = newPositions.find(symbol);
            if (newIt != newPositions.end()) {
                double newSpotSize = newIt->second.first;
                double newContractSize = newIt->second.second;
                double newSizeDiff = std::abs(newSpotSize - newContractSize);
                
                if (newSizeDiff > sizeDiff * 0.001) {
                    logger.warning(symbol + " 重平衡後倉位仍不對等: " +
                                 "現貨=" + std::to_string(newSpotSize) + 
                                 ", 合約=" + std::to_string(newContractSize));
                } else {
                    logger.info(symbol + " 重平衡成功");
                }
            }
        } else {
            logger.info(symbol + " 倉位差異不足,無需重平衡");
        }
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
    
    // 1. 檢查倉位數量是否對等
    logger.info("現貨倉位: " + std::to_string(spotSize) + " USDT");
    logger.info("合約倉位: " + std::to_string(contractSize) + " USDT");
    double sizeDiff = std::abs(spotSize - contractSize);
    const double SIZE_DIFF_THRESHOLD = 0.001;  // 0.1% 誤差容忍度
    bool sizeBalanced = (sizeDiff <= std::min(spotSize, contractSize) * SIZE_DIFF_THRESHOLD);
    
    // 2. 獲取價格資訊
    double spotPrice = exchange.getSpotPrice(symbol);
    double contractPrice = exchange.getContractPrice(symbol);
    if (spotPrice <= 0 || contractPrice <= 0) {
        logger.error("無法獲取 " + symbol + " 價格信息");
        return result;
    }
    logger.info("現貨價格: " + std::to_string(spotPrice) + " USDT");
    logger.info("合約價格: " + std::to_string(contractPrice) + " USDT");
    
    // 3. 計算對衝合約現貨組合倉位價值並檢查是否在允許範圍內
    double spotValue = spotSize * spotPrice;
    double contractValue = contractSize * contractPrice;
    double pairValue = spotValue + contractValue;
    bool valueInRange = (pairValue >= minPositionValue && pairValue <= maxPositionValue);
    logger.info("倉位價值: " + std::to_string(spotValue) + " USDT (現貨), " +
                 std::to_string(contractValue) + " USDT (合約), " + 
                 std::to_string(pairValue) + " USDT (對衝合約現貨組合)");
    logger.info("倉位價值在範圍內: " + std::string(valueInRange ? "是" : "否"));
    
    // 4. 計算價格差異
    result.priceDiff = std::abs(spotPrice - contractPrice) / spotPrice;
    logger.info("價格差異: " + std::to_string(result.priceDiff * 100) + "%");
    
    //預估用的倉位大小
    double predictedSize = 100;
    // 5. 獲取深度資訊並計算影響
    auto orderbook = exchange.getOrderBook(symbol);
    result.depthImpact = calculateDepthImpact(orderbook, predictedSize);
    logger.info("深度影響: " + std::to_string(result.depthImpact * 100) + "%");
    
    // 6. 計算預估成本
    result.estimatedCost = calculateRebalanceCost(symbol, predictedSize);
    logger.info("預估成本: " + std::to_string(result.estimatedCost) + " USDT");
    
    // 7. 計算預期收益
    double fundingRate = exchange.getCurrentFundingRate(symbol);
    result.expectedProfit = calculateExpectedProfit(predictedSize, fundingRate);
    logger.info("預期收益: " + std::to_string(result.expectedProfit) + " USDT");
    
    // 8. 判斷是否需要重平衡
    const double PRICE_DIFF_THRESHOLD = 0.001;    // 0.1% 價格差異閾值
    const double DEPTH_IMPACT_THRESHOLD = 0.0005; // 0.05% 深度影響閾值
    const double MIN_PROFIT_RATIO = 1.5;         // 最小收益/成本比
    
    // 需要重平衡的條件：
    // 1. 倉位數量不對等 或 倉位價值不在允許範圍內
    // 2. 價格差異在可接受範圍內
    // 3. 深度影響在可接受範圍內
    // 4. 預期收益大於成本的2倍
    result.needBalance = 
        (!sizeBalanced || !valueInRange) &&
        (result.priceDiff < PRICE_DIFF_THRESHOLD) &&
        (result.depthImpact < DEPTH_IMPACT_THRESHOLD) &&
        (result.expectedProfit > result.estimatedCost * MIN_PROFIT_RATIO);
    
    // 9. 記錄詳細日誌
    logger.info(symbol + " 倉位檢查結果:");
    logger.info("- 現貨倉位: " + std::to_string(spotSize) + 
               " (" + std::to_string(spotValue) + " USDT)");
    logger.info("- 合約倉位: " + std::to_string(contractSize) + 
               " (" + std::to_string(contractValue) + " USDT)");
    logger.info("- 倉位數量對等: " + std::string(sizeBalanced ? "是" : "否") + 
               " (差異: " + std::to_string(sizeDiff) + ")");
    logger.info("- 倉位價值在範圍內: " + std::string(valueInRange ? "是" : "否") + 
               " [" + std::to_string(minPositionValue) + "," + 
               std::to_string(maxPositionValue) + "]");
    logger.info("- 價格差異: " + std::to_string(result.priceDiff * 100) + "%");
    logger.info("- 深度影響: " + std::to_string(result.depthImpact * 100) + "%");
    logger.info("- 預估成本: " + std::to_string(result.estimatedCost) + " USDT");
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
double TradingModule::calculateRebalanceCost(const std::string& symbol, double size) {
    // 獲取交易費率
    double spotFeeRate = exchange.getSpotFeeRate();
    double contractFeeRate = exchange.getContractFeeRate();
    logger.info("現貨手續費率: " + std::to_string(spotFeeRate));
    logger.info("合約手續費率: " + std::to_string(contractFeeRate));
    
    // 獲取當前價格
    double currentPrice = exchange.getSpotPrice(symbol);
    
    // 計算總成本（包括手續費和滑點）
    double tradingFee = size * currentPrice * (spotFeeRate + contractFeeRate);
    double slippage = size * currentPrice * 0.0005; // 假設0.05%的滑點
    logger.info("交易費用: " + std::to_string(tradingFee));
    logger.info("滑點: " + std::to_string(slippage));
    
    return tradingFee + slippage;
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
