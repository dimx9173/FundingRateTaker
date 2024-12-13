#include "trading/trading_module.h"
#include "config.h"
#include <iostream>
#include <ctime>

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
    double totalInvestment = config.getTotalInvestment();
    double availableEquity = exchange.getTotalEquity();
    
    if (availableEquity <= 0 || availableEquity < totalInvestment * 0.1) {
        std::cout << "可用資金不足" << std::endl;
        return 0.0;
    }

    double basePosition = totalInvestment / config.getTopPairsCount();
    
    double adjustedPosition = basePosition;
    if (config.getPositionScaling()) {
        double scalingFactor = config.getScalingFactor();
        adjustedPosition = basePosition * (1 + std::abs(rate) * scalingFactor);
    }

    adjustedPosition = std::min(adjustedPosition, 
                               totalInvestment * config.getMaxSinglePositionRisk());
    adjustedPosition = std::max(adjustedPosition, 
                               config.getMinPositionSize() * exchange.getSpotPrice(symbol));
    adjustedPosition = std::min(adjustedPosition, 
                               config.getMaxPositionSize() * exchange.getSpotPrice(symbol));

    return adjustedPosition;
}

bool TradingModule::checkTotalPositionLimit() {
    double totalPositionValue = 0.0;
    auto positions = exchange.getPositions();
    
    for (const auto& pos : positions["result"]["list"]) {
        totalPositionValue += std::stod(pos["positionValue"].asString());
    }

    return totalPositionValue < 
           (Config::getInstance().getTotalInvestment() * 
            Config::getInstance().getMaxTotalPosition());
}

std::vector<std::pair<std::string, double>> TradingModule::getTopFundingRates() {
    auto rates = exchange.getFundingRates();
    
    std::cout << "\n=== 當前資金費率排名 ===" << std::endl;
    for (const auto& [symbol, rate] : rates) {
        std::cout << symbol << ": " << (rate * 100) << "%" << std::endl;
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
    if (!checkTotalPositionLimit()) {
        std::cout << "已達到總倉位限制" << std::endl;
        return;
    }

    for (const auto& [symbol, rate] : topRates) {
        double positionSize = calculatePositionSize(symbol, rate);
        if (positionSize <= 0) continue;

        double spotPrice = exchange.getSpotPrice(symbol);
        if (spotPrice <= 0) continue;

        int leverage = Config::getInstance().getDefaultLeverage();
        double spotQty = positionSize / spotPrice;
        double contractQty = spotQty * leverage;

        std::cout << "\n執行對沖交易: " << symbol << std::endl;
        std::cout << "資金費率: " << (rate * 100) << "%" << std::endl;
        std::cout << "倉位大小: " << positionSize << " USDT" << std::endl;
        std::cout << "現貨數量: " << spotQty << std::endl;
        std::cout << "合約數量: " << contractQty << std::endl;

        bool spotOrderSuccess = exchange.createSpotOrder(symbol, "Buy", spotQty);
        Json::Value contractOrder = exchange.createOrder(symbol, "Sell", contractQty);
        bool contractOrderSuccess = contractOrder["retCode"].asInt() == 0;

        if (spotOrderSuccess && contractOrderSuccess) {
            std::string spotOrderId = "SPOT_" + std::to_string(time(nullptr));
            std::string futuresOrderId = contractOrder["result"]["orderId"].asString();
            
            storage.storeTradeGroup(
                "BYBIT", symbol, spotOrderId, futuresOrderId, leverage
            );
            std::cout << "對沖交易成功: " << symbol << std::endl;
        } else {
            if (spotOrderSuccess) {
                exchange.createSpotOrder(symbol, "Sell", spotQty);
            }
            if (contractOrderSuccess) {
                exchange.closePosition(symbol);
            }
            std::cout << "對沖交易失敗，已平倉: " << symbol << std::endl;
        }
    }
}