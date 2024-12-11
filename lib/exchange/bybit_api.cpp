#include "exchange/bybit_api.h"
#include "config.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>

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
    std::string url = BASE_URL + endpoint;
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
    } else {
        std::cerr << "解析響應失敗: " << response << std::endl;
        return Json::Value();
    }
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
    
    for (const auto& symbol : pairs) {
        std::map<std::string, std::string> params;
        params["symbol"] = symbol;
        params["category"] = "linear";
        
        Json::Value response = makeRequest("/v5/market/funding/history", "GET", params);
        
        if (response.isObject() && response["retCode"].asInt() == 0 && 
            response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
            
            double rate = std::stod(response["result"]["list"][0]["fundingRate"].asString());
            rates.emplace_back(symbol, rate);
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
    std::map<std::string, std::string> params;
    if (!symbol.empty()) {
        params["symbol"] = symbol;
    }
    params["category"] = "linear";
    
    return makeRequest("/v5/position/list", "GET", params);
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
    Json::Value positions = getPositions();
    if (positions.isObject() && positions["retCode"].asInt() == 0 && 
        positions["result"]["list"].isArray()) {
        
        std::cout << "\n當前持倉:" << std::endl;
        for (const auto& pos : positions["result"]["list"]) {
            std::cout << "幣對: " << pos["symbol"].asString() 
                      << ", 方向: " << pos["side"].asString()
                      << ", 數量: " << pos["size"].asString()
                      << ", 槓桿: " << pos["leverage"].asString()
                      << std::endl;
        }
    }
}

