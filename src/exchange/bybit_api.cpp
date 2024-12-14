#include "exchange/bybit_api.h"
#include "config.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "logger.h"

std::mutex BybitAPI::mutex_;
std::unique_ptr<BybitAPI> BybitAPI::instance;

BybitAPI::BybitAPI() : 
    API_KEY(Config::getInstance().getBybitApiKey()),
    API_SECRET(Config::getInstance().getBybitApiSecret()),
    BASE_URL(Config::getInstance().getBybitBaseUrl()) {}

size_t BybitAPI::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string BybitAPI::generateSignature(const std::string& params, const std::string& timestamp) {
    std::string signaturePayload = timestamp + API_KEY + "5000" + params;
    
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen;
    
    HMAC(EVP_sha256(), 
         API_SECRET.c_str(), 
         API_SECRET.length(),
         (unsigned char*)signaturePayload.c_str(), 
         signaturePayload.length(), 
         digest, 
         &digestLen);
    
    std::stringstream ss;
    for(unsigned int i = 0; i < digestLen; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    
    return ss.str();
}

Json::Value BybitAPI::makeRequest(const std::string& endpoint, const std::string& method, 
                                  const std::map<std::string, std::string>& params) {
    CURL* curl = curl_easy_init();
    std::string response;
    
    std::string url = BASE_URL;
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += endpoint;
    std::string paramString;
    
    if (curl) {
        auto now = std::chrono::system_clock::now();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        std::string timestamp = std::to_string(millis);
        
        if (method == "GET") {
            for (const auto& [key, value] : params) {
                if (!paramString.empty()) paramString += "&";
                paramString += key + "=" + value;
            }
            if (!paramString.empty()) {
                url += "?" + paramString;
            }
        } else if (method == "POST") {
            Json::Value jsonParams;
            for (const auto& [key, value] : params) {
                jsonParams[key] = value;
            }
            Json::FastWriter writer;
            paramString = writer.write(jsonParams);
            if (!paramString.empty() && paramString[paramString.length()-1] == '\n') {
                paramString.erase(paramString.length()-1);
            }
        }

        std::string signature = generateSignature(paramString, timestamp);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + API_KEY).c_str());
        headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + signature).c_str());
        headers = curl_slist_append(headers, "X-BAPI-RECV-WINDOW: 5000");
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!paramString.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, paramString.c_str());
            }
        }
        
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            std::cerr << "Curl request failed: " << curl_easy_strerror(res) << std::endl;
            return Json::Value();
        }
    }
    
    Json::Value root;
    Json::Reader reader;
    if (!response.empty() && reader.parse(response, root)) {
        if (root.isObject() && root.isMember("retCode") && root["retCode"].asInt() != 0) {
            std::cout << "API錯誤: " << root["retMsg"].asString() << std::endl;
        }
        return root;
    }
    
    return Json::Value();
}

BybitAPI& BybitAPI::getInstance() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!instance) {
        instance.reset(new BybitAPI());
    }
    return *instance;
}

std::vector<std::pair<std::string, double>> BybitAPI::getFundingRates() {
    std::vector<std::pair<std::string, double>> rates;
    auto pairs = Config::getInstance().getTradingPairs();
    auto periods = Config::getInstance().getFundingPeriods();
    auto weights = Config::getInstance().getFundingWeights();
    
    Logger logger;
    logger.info("開始獲取資金費率，交易對數量: " + std::to_string(pairs.size()));
    
    if (periods.empty() || weights.empty() || periods.size() != weights.size()) {
        logger.error("錯誤: 資金費率計算週期或權重配置不正確");
        return rates;
    }
    
    int maxPeriod = *std::max_element(periods.begin(), periods.end());
    
    for (const auto& symbol : pairs) {
        logger.info("處理交易對: " + symbol);
        
        std::map<std::string, std::string> params;
        params["symbol"] = symbol;
        params["category"] = "linear";
        params["limit"] = std::to_string(maxPeriod);
        
        try {
            Json::Value response = makeRequest("/v5/market/funding/history", "GET", params);
            
            if (!response.isObject() || response["retCode"].asInt() != 0 || 
                !response["result"]["list"].isArray() || response["result"]["list"].empty()) {
                logger.error("獲取資金費率失敗: " + symbol);
                continue;
            }
            
            const Json::Value& list = response["result"]["list"];
            double weightedScore = 0.0;
            double totalWeight = 0.0;
            
            // 對每個週期計算加權平均
            for (size_t i = 0; i < periods.size(); i++) {
                int periodLimit = periods[i];
                double periodSum = 0.0;
                int validCount = 0;
                
                // 計算當前週期的平均值
                for (int j = 0; j < list.size() && j < periodLimit; j++) {
                    try {
                        double rate = std::stod(list[j]["fundingRate"].asString());
                        periodSum += rate;
                        validCount++;
                    } catch (const std::exception& e) {
                        logger.error("解析資金費率失敗: " + std::string(e.what()));
                        continue;
                    }
                }
                
                if (validCount > 0) {
                    double periodAvg = periodSum / validCount;
                    weightedScore += periodAvg * weights[i];
                    totalWeight += weights[i];
                }
            }
            
            if (totalWeight > 0) {
                double finalScore = weightedScore / totalWeight;
                rates.emplace_back(symbol, finalScore);
                logger.info(symbol + " 最終加權分數: " + std::to_string(finalScore));
            }
            
        } catch (const std::exception& e) {
            logger.error("處理資金費率時發生異常: " + symbol + " - " + e.what());
            continue;
        }
    }
    
    return rates;
}

bool BybitAPI::setLeverage(const std::string& symbol, int leverage) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["buyLeverage"] = std::to_string(leverage);
    params["sellLeverage"] = std::to_string(leverage);
    params["category"] = "linear";
    
    Json::Value response = makeRequest("/v5/position/set-leverage", "POST", params);
    return response.isObject() && response["retCode"].asInt() == 0;
}

Json::Value BybitAPI::createOrder(const std::string& symbol, const std::string& side, double qty,
                                const std::string& category, const std::string& orderType) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["side"] = side;
    params["orderType"] = orderType;
    params["qty"] = std::to_string(qty);
    params["category"] = category;
    
    return makeRequest("/v5/order/create", "POST", params);
}

bool BybitAPI::createSpotOrder(const std::string& symbol, const std::string& side, double qty) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["side"] = side;
    params["orderType"] = "MARKET";
    params["qty"] = std::to_string(qty);
    params["category"] = "spot";
    
    Json::Value response = makeRequest("/v5/order/create", "POST", params);
    return response.isObject() && response["retCode"].asInt() == 0;
}

void BybitAPI::closePosition(const std::string& symbol) {
    // 先獲取當前持倉
    auto position = getPositions(symbol);
    if (position.isNull() || !position["result"]["list"].isArray() || 
        position["result"]["list"].empty()) {
        return;
    }
    
    double size = std::stod(position["result"]["list"][0]["size"].asString());
    std::string side = position["result"]["list"][0]["side"].asString();
    
    // 創建相反方向的訂單來平倉
    std::string closeSide = (side == "Buy") ? "Sell" : "Buy";
    createOrder(symbol, closeSide, size);
}

Json::Value BybitAPI::getPositions(const std::string& symbol) {
    Logger logger;
    std::map<std::string, std::string> params;
    params["category"] = "linear";
    params["settleCoin"] = "USDT";
    
    if (!symbol.empty()) {
        logger.info("獲取指定幣對倉位: " + symbol);
        params["symbol"] = symbol;
    } else {
        logger.info("獲取所有倉位");
    }
    
    std::string paramString = "category=" + params["category"] + 
                            "&settleCoin=" + params["settleCoin"];
    if (!symbol.empty()) {
        paramString += "&symbol=" + symbol;
    }
    logger.info("請求參數: " + paramString);
    
    Json::Value response = makeRequest("/v5/position/list", "GET", params);
    
    if (!response.isObject()) {
        logger.error("API響應格式錯誤");
        return Json::Value();
    }
    
    if (response["retCode"].asInt() != 0) {
        logger.error("API錯誤: " + response["retMsg"].asString());
        logger.error("完整響應: " + Json::FastWriter().write(response));
        return Json::Value();
    }
    
    return response;
}

double BybitAPI::getTotalEquity() {
    Json::Value response = makeRequest("/v5/account/wallet-balance", "GET", 
                                     {{"accountType", "UNIFIED"}});
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        return std::stod(response["result"]["list"][0]["totalEquity"].asString());
    }
    return 0.0;
}

double BybitAPI::getSpotPrice(const std::string& symbol) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["category"] = "spot";
    
    Json::Value response = makeRequest("/v5/market/tickers", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        return std::stod(response["result"]["list"][0]["lastPrice"].asString());
    }
    return 0.0;
}

void BybitAPI::displayPositions() {
    Logger logger;
    
    // 獲取合約倉位
    Json::Value futuresPositions = getPositions();
    
    // 獲取現貨餘額
    std::map<std::string, std::string> params;
    params["accountType"] = "UNIFIED";
    Json::Value spotBalances = makeRequest("/v5/account/wallet-balance", "GET", params);
    
    // 顯示邏輯保持不變
    std::cout << "\n=== 當前持倉狀態 ===" << std::endl;
    std::cout << std::left
              << std::setw(12) << "幣對"
              << std::setw(10) << "類型"
              << std::setw(10) << "方向"
              << std::setw(15) << "數量"
              << std::setw(15) << "未實現盈虧"
              << std::setw(15) << "倉位價值"
              << std::endl;
    std::cout << std::string(77, '-') << std::endl;
    
    double totalValue = 0.0;
    double totalPnL = 0.0;
    
    // 顯示合約倉位
    if (futuresPositions.isObject() && futuresPositions["result"]["list"].isArray()) {
        for (const auto& pos : futuresPositions["result"]["list"]) {
            try {
                double size = std::stod(pos["size"].asString());
                if (size <= 0) continue;
                
                std::cout << std::left
                          << std::setw(12) << pos["symbol"].asString()
                          << std::setw(10) << "合約"
                          << std::setw(10) << pos["side"].asString()
                          << std::setw(15) << std::fixed << std::setprecision(4) << size
                          << std::setw(15) << std::fixed << std::setprecision(2) 
                          << std::stod(pos["unrealisedPnl"].asString())
                          << std::setw(15) << std::fixed << std::setprecision(2) 
                          << std::stod(pos["positionValue"].asString())
                          << std::endl;
                
                totalValue += std::stod(pos["positionValue"].asString());
                totalPnL += std::stod(pos["unrealisedPnl"].asString());
            } catch (const std::exception& e) {
                logger.error("解析合約倉位數據失敗: " + std::string(e.what()));
                continue;
            }
        }
    }
    
    // 修改現貨餘額的解析邏輯
    if (spotBalances.isObject() && spotBalances["result"]["list"].isArray()) {
        const auto& list = spotBalances["result"]["list"][0]["coin"];
        if (list.isArray()) {
            for (const auto& coin : list) {
                try {
                    std::string symbol = coin["coin"].asString();
                    if (symbol == "USDT") continue;
                    
                    double size = std::stod(coin["walletBalance"].asString());
                    if (size <= 0) continue;
                    
                    std::string pairSymbol = symbol + "USDT";
                    double spotPrice = getSpotPrice(pairSymbol);
                    double positionValue = size * spotPrice;
                    
                    if (positionValue > 0) {
                        std::cout << std::left
                                  << std::setw(12) << pairSymbol
                                  << std::setw(10) << "現貨"
                                  << std::setw(10) << "Buy"
                                  << std::setw(15) << std::fixed << std::setprecision(4) << size
                                  << std::setw(15) << std::fixed << std::setprecision(2) << 0.0
                                  << std::setw(15) << std::fixed << std::setprecision(2) << positionValue
                                  << std::endl;
                        
                        totalValue += positionValue;
                    }
                } catch (const std::exception& e) {
                    logger.error("解析現貨餘額數據失敗: " + std::string(e.what()));
                    continue;
                }
            }
        }
    }
    
    // 顯示匯總信息部分保持不變
    std::cout << std::string(77, '-') << std::endl;
    std::cout << "總倉位價值: " << std::fixed << std::setprecision(2) << totalValue << " USDT" << std::endl;
    std::cout << "總未實現盈虧: " << std::fixed << std::setprecision(2) << totalPnL << " USDT" << std::endl;
    
    double equity = getTotalEquity();
    if (equity > 0) {
        std::cout << "賬戶總權益: " << std::fixed << std::setprecision(2) << equity << " USDT" << std::endl;
        double utilizationRate = (totalValue / equity) * 100;
        std::cout << "倉位使用率: " << std::fixed << std::setprecision(2) << utilizationRate << "%" << std::endl;
    }
}

std::vector<std::string> BybitAPI::getInstruments(const std::string& category) {
    std::vector<std::string> instruments;
    std::map<std::string, std::string> params;
    params["category"] = category;
    
    Json::Value response = makeRequest("/v5/market/instruments-info", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray()) {
        
        const Json::Value& list = response["result"]["list"];
        for (const auto& instrument : list) {
            if (instrument["status"].asString() == "Trading") {
                instruments.push_back(instrument["symbol"].asString());
            }
        }
    }
    
    return instruments;
}

Json::Value BybitAPI::getSpotBalances() {
    Logger logger;
    logger.info("獲取現貨餘額");
    
    std::map<std::string, std::string> params;
    params["accountType"] = "UNIFIED";
    
    try {
        Json::Value response = makeRequest("/v5/account/wallet-balance", "GET", params);
        
        if (response["retCode"].asInt() != 0) {
            logger.error("獲取現貨餘額失敗: " + response["retMsg"].asString());
            return Json::Value(Json::objectValue);
        }
        
        return response;
    } catch (const std::exception& e) {
        logger.error("獲取現貨餘額異常: " + std::string(e.what()));
        return Json::Value(Json::objectValue);
    }
}

