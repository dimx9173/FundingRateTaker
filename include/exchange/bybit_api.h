#ifndef BYBIT_API_H
#define BYBIT_API_H

#include <string>
#include <vector>
#include <map>
#include <json/json.h>
#include <mutex>
#include <memory>

class BybitAPI {
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

    std::vector<std::pair<std::string, double>> getFundingRates();
    bool setLeverage(const std::string& symbol, int leverage);
    Json::Value createOrder(const std::string& symbol, const std::string& side, double qty,
                            const std::string& category = "linear", 
                            const std::string& orderType = "Market");
    bool createSpotOrder(const std::string& symbol, const std::string& side, double qty);
    void closePosition(const std::string& symbol);
    void executeHedgeStrategy();
    Json::Value getPositions(const std::string& symbol = "");
    void getWalletBalance();
    void displayPositions();
    double getTotalEquity();
    double getSpotPrice(const std::string& symbol);
};

#endif // BYBIT_API_H
