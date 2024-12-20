#include "exchange/bybit_api.h"
#include "config.h"
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
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
    Logger logger;
    
    // 檢查是否為無效的交易對請求
    auto symbolIter = params.find("symbol");
    if (symbolIter != params.end() && symbolIter->second == "USDTUSDT") {
        logger.error("無效的交易對請求: USDTUSDT");
        Json::Value errorResponse;
        errorResponse["retCode"] = 10001;
        errorResponse["retMsg"] = "Invalid trading pair";
        return errorResponse;
    }
    
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
        
        // 請求參數
        // logger.info("請求URL: " + url);
        // logger.info("請求時間戳: " + timestamp);
        
        if (method == "GET") {
            for (const auto& [key, value] : params) {
                if (!paramString.empty()) paramString += "&";
                paramString += key + "=" + value;
            }
            if (!paramString.empty()) {
                url += "?" + paramString;
                // logger.info("GET參數: " + paramString);
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
            // logger.info("POST數據: " + paramString);
        }

        std::string signature = generateSignature(paramString, timestamp);
        // logger.info("生成的簽名: " + signature);
        
        // 記錄請求頭
        // logger.info("請求頭:");
        // logger.info("X-BAPI-API-KEY: " + API_KEY);
        // logger.info("X-BAPI-TIMESTAMP: " + timestamp);
        // logger.info("X-BAPI-SIGN: " + signature);
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + API_KEY).c_str());
        headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + signature).c_str());
        headers = curl_slist_append(headers, "X-BAPI-RECV-WINDOW: 5000");
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        // 設置CURL選項
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
        
        // 執行請求
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            logger.error("CURL請求失敗: " + std::string(curl_easy_strerror(res)));
            return Json::Value();
        }
        
        // 記錄響應
        // logger.info("API響應: " + response);
    }
    
    // 解析響應
    Json::Value root;
    Json::Reader reader;
    if (!response.empty() && reader.parse(response, root)) {
        if (root.isObject() && root.isMember("retCode")) {
            if (root["retCode"].asInt() != 0) {
                logger.error("API錯誤碼: " + std::to_string(root["retCode"].asInt()));
                logger.error("錯誤信息: " + root["retMsg"].asString());
            } else {
                // logger.info("請求成功完成");
            }
        }
        return root;
    }
    
    logger.error("JSON解析失敗");
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
    int historyDays = Config::getInstance().getFundingHistoryDays();
    
    Logger logger;
    logger.info("開始獲取資金費率歷史數據");
    
    for (const auto& symbol : pairs) {
        std::map<std::string, std::string> params;
        params["symbol"] = symbol;
        params["category"] = "linear";
        params["limit"] = std::to_string(historyDays * 3); // 每天3次資金費率
        
        try {
            Json::Value response = makeRequest("/v5/market/funding/history", "GET", params);
            
            if (!response.isObject() || response["retCode"].asInt() != 0 || 
                !response["result"]["list"].isArray()) {
                continue;
            }
            
            // 只取最新的一個資金費率
            const Json::Value& list = response["result"]["list"];
            if (!list.empty()) {
                try {
                    double rate = std::stod(list[0]["fundingRate"].asString());
                    rates.emplace_back(symbol, rate);
                } catch (const std::exception& e) {
                    logger.error("解析資金費率失敗: " + symbol);
                    continue;
                }
            }
            
        } catch (const std::exception& e) {
            logger.error("處理資金費率時發生異常: " + symbol);
            continue;
        }
    }
    
    return rates;
}

std::vector<std::pair<std::string, std::vector<double>>> BybitAPI::getFundingHistory(
    const std::vector<std::string>& targetSymbols) {
    
    std::vector<std::pair<std::string, std::vector<double>>> rates;
    
    int historyDays = Config::getInstance().getFundingHistoryDays();
    Logger logger;
    logger.info("開始獲取資金費率歷史數據,"+std::to_string(targetSymbols.size()));
    
    for (const auto& symbol : targetSymbols) {
        std::map<std::string, std::string> params;
        params["symbol"] = symbol;
        params["category"] = "linear";
        params["limit"] = std::to_string(historyDays * 3);
        
        try {
            Json::Value response = makeRequest("/v5/market/funding/history", "GET", params);
            
            if (!response.isObject() || response["retCode"].asInt() != 0 || 
                !response["result"]["list"].isArray()) {
                logger.error("獲取" + symbol + "資金費率歷史失敗");
                continue;
            }
            
            std::vector<double> symbolRates;
            const Json::Value& list = response["result"]["list"];
            
            for (const auto& rate : list) {
                try {
                    double fundingRate = std::stod(rate["fundingRate"].asString());
                    symbolRates.push_back(fundingRate);
                } catch (const std::exception& e) {
                    logger.error("解析資金費率失敗: " + symbol + " - " + e.what());
                    continue;
                }
            }
            
            if (!symbolRates.empty()) {
                rates.emplace_back(symbol, symbolRates);
            }
            
        } catch (const std::exception& e) {
            logger.error("處理資金費率歷史時發生異常: " + symbol + " - " + e.what());
            continue;
        }
        
        // 添加短暫延遲以避免API請求過於頻繁
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    
    Json::Value response = makeRequest("/v5/order/create", "POST", params);
    if (response["retCode"].asInt() != 0) {
        lastError = response["retMsg"].asString();
    }
    
    return response;
}

bool BybitAPI::createSpotOrder(const std::string& symbol, const std::string& side, double qty) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["side"] = side;
    params["orderType"] = "MARKET";
    params["qty"] = std::to_string(qty); //現貨倉位需要整數
    params["category"] = "spot";
    params["marketUnit"] = "baseCoin";
    
    Json::Value response = makeRequest("/v5/order/create", "POST", params);
    if (response["retCode"].asInt() != 0) {
        lastError = response["retMsg"].asString();
        return false;
    }
    return true;
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
    // logger.info("獲取現貨餘額");
    
    std::map<std::string, std::string> params;
    params["accountType"] = "UNIFIED";
    
    try {
        Json::Value response = makeRequest("/v5/account/wallet-balance", "GET", params);
        // logger.info("現貨餘額: " + Json::FastWriter().write(response));
        
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

double BybitAPI::getSpotBalance(const std::string& symbol) {
    Logger logger;
    Json::Value spotBalances = getSpotBalances();
    
    // 從 symbol 中提取幣種名稱（例如從 "BTCUSDT" 提取 "BTC"）
    std::string coin = symbol.substr(0, symbol.length() - 4); // 假設都是 XXXUSDT 格式
    
    if (spotBalances.isObject() && 
        spotBalances["result"]["list"].isArray() && 
        spotBalances["result"]["list"][0]["coin"].isArray()) {
        
        const Json::Value& coins = spotBalances["result"]["list"][0]["coin"];
        for (const auto& coinData : coins) {
            if (coinData["coin"].asString() == coin) {
                double balance = std::stod(coinData["walletBalance"].asString());
                logger.info(coin + " 現貨餘額: " + std::to_string(balance));
                return balance;
            }
        }
    }
    
    logger.error("未找到 " + coin + " 的餘額");
    return 0.0;
}

std::string BybitAPI::getLastError() {
    return lastError;
}

// 獲取合約價格
double BybitAPI::getContractPrice(const std::string& symbol) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["category"] = "linear";
    
    Json::Value response = makeRequest("/v5/market/tickers", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        return std::stod(response["result"]["list"][0]["lastPrice"].asString());
    }
    return 0.0;
}

// 獲取訂單簿
Json::Value BybitAPI::getSpotOrderBook(const std::string& symbol) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["category"] = "spot";
    params["limit"] = "50";  // 獲取前50層深度
    
    Json::Value response = makeRequest("/v5/market/orderbook", "GET", params);
    
    // 添加日誌輸出查看數據
    // Logger logger;
    // logger.info("訂單簿原始數據: " + Json::FastWriter().write(response));
    
    return response;
}

Json::Value BybitAPI::getContractOrderBook(const std::string& symbol) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["category"] = "linear";
    params["limit"] = "50";  // 獲取前50層深度
    
    Json::Value response = makeRequest("/v5/market/orderbook", "GET", params);
        // 添加日誌輸出查看數據
    // Logger logger;
    // logger.info("訂單簿原始數據: " + Json::FastWriter().write(response));
    return response;
}

// 獲取當前資金費率
double BybitAPI::getCurrentFundingRate(const std::string& symbol) {
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    params["category"] = "linear";
    
    Json::Value response = makeRequest("/v5/market/tickers", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        return std::stod(response["result"]["list"][0]["fundingRate"].asString());
    }
    return 0.0;
}

// 獲取現貨手續費率
double BybitAPI::getSpotFeeRate() {
    Logger logger;
    std::map<std::string, std::string> params;
    params["category"] = "spot";

    Json::Value response = makeRequest("/v5/account/fee-rate", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        try {
            // 獲取taker費率作為保守估計
            double takerFeeRate = std::stod(response["result"]["list"][0]["takerFeeRate"].asString());
            return takerFeeRate;
        } catch (const std::exception& e) {
            logger.error("解析現貨手續費率失敗: " + std::string(e.what()));
        }
    }
    
    // 如果API請求失敗，返回預設值
    return 0.001; // 0.1% 作為預設值
}

// 獲取合約手續費率
double BybitAPI::getContractFeeRate() {
    Logger logger;
    std::map<std::string, std::string> params;
    params["category"] = "linear";

    Json::Value response = makeRequest("/v5/account/fee-rate", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        try {
            // 獲取taker費率作為保守估計
            double takerFeeRate = std::stod(response["result"]["list"][0]["takerFeeRate"].asString());
            return takerFeeRate;
        } catch (const std::exception& e) {
            logger.error("解析合約手續費率失敗: " + std::string(e.what()));
        }
    }
    
    // 如果API請求失敗，返回預設值
    return 0.0006; // 0.06% 作為預設值
}

double BybitAPI::getMarginRatio(const std::string& symbol) {
    // 從API獲取保證金率
    std::map<std::string, std::string> params;
    params["symbol"] = symbol;
    
    Json::Value response = makeRequest("/v5/account/collateral-info", "GET", params);
    
    if (response.isObject() && response["retCode"].asInt() == 0 && 
        response["result"]["list"].isArray() && !response["result"]["list"].empty()) {
        return std::stod(response["result"]["list"][0]["collateralRatio"].asString());
    }
    
    // 如果無法獲取，返回默認值
    return 0.8; // 默認80%保證金率
}
