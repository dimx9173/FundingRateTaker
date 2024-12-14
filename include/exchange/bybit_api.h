#ifndef BYBIT_API_H
#define BYBIT_API_H

#include "exchange_interface.h"
#include <mutex>
#include <memory>

class BybitAPI : public IExchange {
private:
    static std::mutex mutex_;
    static std::unique_ptr<BybitAPI> instance;
    const std::string API_KEY;
    const std::string API_SECRET;
    const std::string BASE_URL;

    BybitAPI();

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);
    std::string generateSignature(const std::string& params, const std::string& timestamp);
    Json::Value makeRequest(const std::string& endpoint, const std::string& method, 
                          const std::map<std::string, std::string>& params = {});

public:
    static BybitAPI& getInstance();

    // 實現 IExchange 介面
    std::vector<std::pair<std::string, double>> getFundingRates() override;
    double getSpotPrice(const std::string& symbol) override;
    double getTotalEquity() override;
    Json::Value getPositions(const std::string& symbol = "") override;
    bool setLeverage(const std::string& symbol, int leverage) override;
    Json::Value createOrder(const std::string& symbol, 
                          const std::string& side, 
                          double qty,
                          const std::string& category = "linear",
                          const std::string& orderType = "Market") override;
    bool createSpotOrder(const std::string& symbol, 
                        const std::string& side, 
                        double qty) override;
    void closePosition(const std::string& symbol) override;
    void displayPositions() override;
    std::vector<std::string> getInstruments(const std::string& category = "linear") override;
    Json::Value getSpotBalances() override;
};

#endif // BYBIT_API_H
