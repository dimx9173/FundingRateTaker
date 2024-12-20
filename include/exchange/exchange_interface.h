#ifndef EXCHANGE_INTERFACE_H
#define EXCHANGE_INTERFACE_H

#include <string>
#include <vector>
#include <utility>
#include <json/json.h>

class IExchange {
public:
    virtual ~IExchange() = default;

    // 基本市場數據
    virtual std::vector<std::pair<std::string, double>> getFundingRates() = 0;
    virtual double getSpotPrice(const std::string& symbol) = 0;
    virtual double getTotalEquity() = 0;
    virtual Json::Value getPositions(const std::string& symbol = "") = 0;
    virtual std::vector<std::string> getInstruments(const std::string& category = "linear") = 0;

    // 交易操作
    virtual bool setLeverage(const std::string& symbol, int leverage) = 0;
    virtual Json::Value createOrder(const std::string& symbol, 
                                  const std::string& side, 
                                  double qty,
                                  const std::string& category = "linear",
                                  const std::string& orderType = "Market") = 0;
    virtual bool createSpotOrder(const std::string& symbol, 
                               const std::string& side, 
                               double qty) = 0;
    virtual void closePosition(const std::string& symbol) = 0;
    virtual std::string getLastError() = 0;

    // 新方法
    virtual Json::Value getSpotBalances() = 0;
    virtual double getSpotBalance(const std::string& symbol) = 0;
    virtual std::vector<std::pair<std::string, std::vector<double>>> getFundingHistory(
        const std::vector<std::string>& symbols = {}) = 0;
    virtual double getContractPrice(const std::string& symbol) = 0;
    virtual Json::Value getSpotOrderBook(const std::string& symbol) = 0;
    virtual Json::Value getContractOrderBook(const std::string& symbol) = 0;
    virtual double getCurrentFundingRate(const std::string& symbol) = 0;
    virtual double getSpotFeeRate() = 0;
    virtual double getContractFeeRate() = 0;
    virtual double getMarginRatio(const std::string& symbol) = 0;
};

#endif // EXCHANGE_INTERFACE_H 