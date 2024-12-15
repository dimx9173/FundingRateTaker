#include "trading/trading_module.h"
#include "config.h"
#include <iostream>
#include <ctime>
#include <iomanip>
#include <thread>
#include <chrono>

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
    quantity = adjustPrecision(quantity, symbol);
    
    // 確保最終倉位價值不小於最小要求
    double finalValue = quantity * currentPrice;
    if (finalValue < minPositionValue) {
        return 0.0;  // 放棄該交易
    }

    return finalValue;
}

std::vector<std::pair<std::string, double>> TradingModule::getTopFundingRates() {
    if (isNearSettlement()) {
        std::cout << "接近結算時間，重新計算資金費率..." << std::endl;
    }
    
    auto rates = exchange.getFundingRates();
    auto periods = Config::getInstance().getFundingPeriods();
    auto weights = Config::getInstance().getFundingWeights();
    
    std::cout << "\n=== 資金費率加權排名 ===" << std::endl;
    std::cout << "計算週期(小時): ";
    for (size_t i = 0; i < periods.size(); i++) {
        std::cout << periods[i] << (i < periods.size() - 1 ? ", " : "");
    }
    std::cout << "\n權重: ";
    for (size_t i = 0; i < weights.size(); i++) {
        std::cout << weights[i] << (i < weights.size() - 1 ? ", " : "");
    }
    std::cout << std::endl;
    
    for (const auto& [symbol, rate] : rates) {
        std::cout << symbol << ": " << (rate * 100) << "% (加權)" << std::endl;
    }
    
    return rates;
}

void TradingModule::closeTradeGroup(const std::string& group) {
    auto parts = splitString(group, ":");
    if (parts.size() >= 2) {
        std::string symbol = parts[1];
        exchange.closePosition(symbol);
    }
}

void TradingModule::executeHedgeStrategy(const std::vector<std::pair<std::string, double>>& topRates) {
    logger.info("開始執行對沖策略");
    logger.info("當前排名交易對數量: " + std::to_string(topRates.size()));
    
    auto positions = exchange.getPositions();
    auto spotBalances = exchange.getSpotBalances();
    
    logger.info("獲取當前倉位信息完成");
    
    // 檢查並調整合約和現貨倉位
    std::map<std::string, std::pair<double, double>> positionSizes; // <symbol, <spot_size, contract_size>>
    
    // 計算現貨數量
    if (spotBalances["result"]["list"].isArray()) {
        const auto& list = spotBalances["result"]["list"][0]["coin"];
        if (list.isArray()) {
            for (const auto& coin : list) {
                std::string symbol = coin["coin"].asString() + "USDT";
                double size = std::stod(coin["walletBalance"].asString());
                if (size > 0) {
                    positionSizes[symbol].first = size;
                }
            }
        }
    }
    
    // 計算合約數量
    if (positions["result"]["list"].isArray()) {
        for (const auto& pos : positions["result"]["list"]) {
            std::string symbol = pos["symbol"].asString();
            double size = std::stod(pos["size"].asString());
            positionSizes[symbol].second = size;
        }
    }
    
    // 直接進入原有的倉位處理邏輯
    auto currentSymbols = getCurrentPositionSymbols();
    
    // 檢查現有倉位是否需要平倉
    for (const auto& symbol : currentSymbols) {
        if (shouldClosePosition(symbol, topRates)) {
            logger.info("準備關閉倉位: " + symbol);
            
            // 獲取現貨和合約倉位數量
            double spotSize = 0.0;
            double contractSize = 0.0;
            
            // 獲取現貨倉位
            auto spotBalances = exchange.getSpotBalances();
            if (spotBalances["result"]["list"].isArray()) {
                const auto& list = spotBalances["result"]["list"][0]["coin"];
                if (list.isArray()) {
                    for (const auto& coin : list) {
                        std::string coinSymbol = coin["coin"].asString() + "USDT";
                        if (coinSymbol == symbol) {
                            double balance = std::stod(coin["walletBalance"].asString());
                            // 檢查是否大於最小訂單數量
                            if (balance >= getMinOrderSize(symbol)) {
                                spotSize = balance;
                            } else {
                                spotSize = 0.0;  // 小於最小訂單數量視為無倉位
                                std::cout << "現貨數量小於最小訂單數量，視為無倉位: " << symbol 
                                        << " 數量: " << balance 
                                        << " 最小數量: " << getMinOrderSize(symbol) << std::endl;
                            }
                            break;
                        }
                    }
                }
            }
            
            // 獲合約倉位
            auto positions = exchange.getPositions();
            if (positions["result"]["list"].isArray()) {
                for (const auto& pos : positions["result"]["list"]) {
                    if (pos["symbol"].asString() == symbol) {
                        contractSize = std::stod(pos["size"].asString());
                        break;
                    }
                }
            }
            
            // 同時關閉現貨和合約倉位
            if (spotSize > 0) {
                spotSize = adjustPrecision(spotSize, symbol);
                // 如果現貨數量太小，直接跳過
                if (spotSize < getMinOrderSize(symbol)) {
                    std::cout << "現貨數量太小，跳過: " << symbol 
                              << " 數量: " << spotSize 
                              << " 最小數量: " << getMinOrderSize(symbol) << std::endl;
                    continue;
                }
                
                bool success = exchange.createSpotOrder(symbol, "Sell", spotSize);
                if (!success) {
                    std::cout << "關閉現貨倉位失��: " << symbol << std::endl;
                }
                logger.info(symbol + " 現貨關閉數量: " + std::to_string(spotSize));
            }

            if (contractSize > 0) {
                contractSize = adjustPrecision(contractSize, symbol);
                if (contractSize >= getMinOrderSize(symbol)) {
                    Json::Value result = exchange.createOrder(symbol, "Sell", contractSize, "linear", "MARKET");
                    bool success = result["retCode"].asInt() == 0;
                    if (!success) {
                        std::cout << "關閉合約倉位失敗: " << symbol << std::endl;
                    }
                    logger.info(symbol + " 合約關閉數量: " + std::to_string(contractSize));
                }
            }
        }
    }
    
    // 處理新的排行倉位
    for (const auto& [symbol, rate] : topRates) {
        logger.info("檢查新倉位: " + symbol + " 費率: " + std::to_string(rate));
        
        // 檢查是否已經有該幣對的倉位
        bool hasPosition = std::find(currentSymbols.begin(), 
                                   currentSymbols.end(), 
                                   symbol) != currentSymbols.end();
        if (hasPosition) {
            std::cout << "已有倉位: " << symbol << std::endl;
            continue;
        }
        
        double positionSize = calculatePositionSize(symbol, rate);
        if (positionSize <= 0) continue;

        double spotPrice = exchange.getSpotPrice(symbol);
        if (spotPrice <= 0) continue;

        int leverage = Config::getInstance().getDefaultLeverage();
        double spotQty = positionSize / spotPrice;
        double contractQty = spotQty;

        std::cout << "\n執行對衝交易: " << symbol << std::endl;
        std::cout << "資金費率: " << (rate * 100) << "%" << std::endl;
        std::cout << "倉位大小: " << positionSize << " USDT" << std::endl;
        std::cout << "現貨數量: " << spotQty << std::endl;
        std::cout << "合約數量: " << contractQty << std::endl;

        // 檢查現貨餘額
        double spotBalance = 0.0;
        if (spotBalances["result"]["list"].isArray()) {
            const auto& list = spotBalances["result"]["list"][0]["coin"];
            if (list.isArray()) {
                for (const auto& coin : list) {
                    if (coin["coin"].asString() == "USDT") {
                        spotBalance = std::stod(coin["walletBalance"].asString());
                        break;
                    }
                }
            }
        }

        // 檢查餘額是否足夠
        double requiredBalance = positionSize * 1.01;  // 加1%作為續費緩衝
        if (spotBalance < requiredBalance) {
            std::cout << "現貨餘額不足，跳過交易: " << symbol 
                      << "\n需要: " << requiredBalance 
                      << " USDT\n當前: " << spotBalance 
                      << " USDT" << std::endl;
            continue;
        }

        // 先執行現貨買入
        bool spotOrderSuccess = exchange.createSpotOrder(symbol, "Buy", spotQty);
        if (!spotOrderSuccess) {
            std::cout << "現貨入失敗，跳過交易: " << symbol << std::endl;
            continue;
        }

        // 現貨買入成功後，執行合約賣出（做空）
        Json::Value contractOrder = exchange.createOrder(symbol, "Sell", contractQty, "linear", "MARKET");
        bool contractOrderSuccess = contractOrder["retCode"].asInt() == 0;

        if (!contractOrderSuccess) {
            // 如果合約失敗，需要賣出剛買入的現貨
            std::cout << "合約賣出敗，回滾現貨交易: " << symbol << std::endl;
            exchange.createSpotOrder(symbol, "Sell", spotQty);
            continue;
        }

        std::string spotOrderId = "SPOT_" + std::to_string(time(nullptr));
        std::string futuresOrderId = contractOrder["result"]["orderId"].asString();
        
        storage.storeTradeGroup(
            "BYBIT", symbol, spotOrderId, futuresOrderId, leverage
        );
        std::cout << "對衝交易成功: " << symbol << std::endl;
    }

    // 檢查倉位平衡
    for (const auto& [symbol, rate] : topRates) {
        // 檢查是否有該幣對的倉位
        if (positionSizes.find(symbol) == positionSizes.end()) {
            // 計算當前總投資額
            double totalInvestment = 0.0;
            for (const auto& [sym, sizes] : positionSizes) {
                double spotPrice = exchange.getSpotPrice(sym);
                if (spotPrice > 0) {
                    totalInvestment += sizes.first * spotPrice;  // 現貨價值
                }
            }
            
            // 獲取當前價格
            double currentPrice = exchange.getSpotPrice(symbol);
            if (currentPrice <= 0) {
                std::cout << "無法獲取" << symbol << "價格，跳過" << std::endl;
                continue;
            }
            
            // 檢查是否超過總投資限制
            double maxTotalInvestment = Config::getInstance().getTotalInvestment();
            if (totalInvestment >= maxTotalInvestment) {
                std::cout << "已達到總投資限制，跳過新建倉位: " << symbol << std::endl;
                continue;
            }
            
            // 使用配置中的最小和最大倉位價值
            double minPositionValue = Config::getInstance().getMinPositionValue();
            double maxPositionValue = Config::getInstance().getMaxPositionValue();
            
            // 基礎倉位計算
            double basePosition = minPositionValue;
            
            // 根據資金費率調整倉位大小
            double adjustedPosition = basePosition;
            if (Config::getInstance().getPositionScaling()) {
                double scalingFactor = Config::getInstance().getScalingFactor();
                adjustedPosition = basePosition * (1 + std::abs(rate) * scalingFactor);
                // 限制單個交易對的最大倉位價值
                adjustedPosition = std::min(adjustedPosition, maxPositionValue);
            }
            
            // 確保不超過剩餘可投資額度
            double remainingInvestment = maxTotalInvestment - totalInvestment;
            adjustedPosition = std::min(adjustedPosition, remainingInvestment);
            
            // 計算初始數量
            double initialSize = adjustedPosition / currentPrice;
            initialSize = adjustPrecision(initialSize, symbol);
            
            // 確保數量大於最小訂單量
            double minSize = getMinOrderSize(symbol);
            if (initialSize < minSize) {
                std::cout << "計算後的倉位數量小於最小訂單量，跳過: " << symbol << std::endl;
                continue;
            }
            
            std::cout << "\n=== 建立新對衝倉位 " << symbol << " ===" << std::endl;
            std::cout << "當前總投資額: " << totalInvestment << " USDT" << std::endl;
            std::cout << "剩餘可投資額: " << remainingInvestment << " USDT" << std::endl;
            std::cout << "目標倉位價值: " << adjustedPosition << " USDT" << std::endl;
            std::cout << "初始數量: " << initialSize << std::endl;
            
            // 先建立現貨多倉
            bool spotSuccess = exchange.createSpotOrder(symbol, "Buy", initialSize);
            if (!spotSuccess) {
                std::cout << "建立現貨倉位失敗，跳過" << std::endl;
                continue;
            }
            
            // 等待現貨訂單完成
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            // 再建立合約空倉
            auto contractResult = exchange.createOrder(symbol, "Sell", initialSize, "linear", "MARKET");
            if (contractResult["retCode"].asInt() != 0) {
                std::cout << "建立合約倉位失敗: " << contractResult["retMsg"].asString() << std::endl;
                continue;
            }
            
            std::cout << "成功建立對衝倉位" << std::endl;
            continue;
        }

        double spotSize = positionSizes[symbol].first;
        double contractSize = positionSizes[symbol].second;
        double sizeDiff = std::abs(spotSize - contractSize);
        
        std::cout << "\n=== 檢查倉位平衡 " << symbol << " ===" << std::endl;
        std::cout << "現貨數量: " << std::fixed << std::setprecision(8) << spotSize << std::endl;
        std::cout << "合約數量: " << std::fixed << std::setprecision(8) << contractSize << std::endl;
        std::cout << "數量差異: " << std::fixed << std::setprecision(8) << sizeDiff << std::endl;
        
        // 調整數量精度
        sizeDiff = adjustPrecision(sizeDiff, symbol);
        
        if (sizeDiff >= getMinOrderSize(symbol)) {
            if (spotSize > contractSize) {
                // 增加合約空倉
                std::cout << "執行調整: 增加合約空倉 " << sizeDiff << " " << symbol << std::endl;
                auto contractResult = exchange.createOrder(symbol, "Sell", sizeDiff, "linear", "MARKET");
                if (contractResult["retCode"].asInt() == 0) {
                    std::cout << "合約賣出成功" << std::endl;
                } else {
                    std::cout << "合約賣出失敗: " << contractResult["retMsg"].asString() << std::endl;
                }
            } else {
                // 減少合約空倉
                std::cout << "執行調整: 減少合約空倉 " << sizeDiff << " " << symbol << std::endl;
                auto contractResult = exchange.createOrder(symbol, "Buy", sizeDiff, "linear", "MARKET");
                if (contractResult["retCode"].asInt() == 0) {
                    std::cout << "合約買入成功" << std::endl;
                } else {
                    std::cout << "合約買入失敗: " << contractResult["retMsg"].asString() << std::endl;
                }
            }
        } else {
            std::cout << "數量差異小於最小訂單數量，無需調整" << std::endl;
        }
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
    
    // 如果不在排行中，直接平倉
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

// 精度的輔助函數
double TradingModule::adjustPrecision(double quantity, const std::string& symbol) {
    double currentPrice = exchange.getSpotPrice(symbol);
    
    if (currentPrice <= 0) {
        return 0.0;  // 如果無法獲取價格，返回0
    }
    
    // 根據幣價動態調整精度
    if (currentPrice >= 10000.0) {  // BTC等高價幣
        return std::floor(quantity * 1000) / 1000.0;  // 0.001
    } else if (currentPrice >= 1000.0) {  // ETH等中高價幣
        return std::floor(quantity * 100) / 100.0;    // 0.01
    } else if (currentPrice >= 100.0) {   // BNB等中價幣
        return std::floor(quantity * 10) / 10.0;      // 0.1
    } else {                              // 其他低價幣
        return std::floor(quantity);                  // 1.0
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
        // 對於10-100美元的幣種，保留兩位小數
        return std::ceil(minSize * 100) / 100.0;
    } else {
        // 對於高價幣種，保留三位小數
        return std::ceil(minSize * 1000) / 1000.0;
    }
}