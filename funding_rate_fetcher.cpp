#include <iostream>
#include <string>
#include <curl/curl.h>
#include <json/json.h>
#include <algorithm>
#include <vector>
#include <map>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

class ConfigLoader {
public:
    static std::string loadApiKey() {
        std::ifstream file("config.json");
        if (!file.is_open()) {
            throw std::runtime_error("無法打開配置文件");
        }

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(file, root)) {
            throw std::runtime_error("解析配置文件失敗");
        }

        if (!root.isMember("coinmarketcap_api_key")) {
            throw std::runtime_error("配置文件中未找到 API key");
        }

        return root["coinmarketcap_api_key"].asString();
    }
};

class CoinMarketCapAPI {
private:
    std::string apiKey;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    CoinMarketCapAPI() {
        try {
            apiKey = ConfigLoader::loadApiKey();
        } catch (const std::exception& e) {
            std::cerr << "載入 API key 失敗: " << e.what() << std::endl;
            throw;
        }
    }

    std::vector<std::string> getTop20Cryptocurrencies() {
        std::vector<std::string> symbols;
        CURL* curl = curl_easy_init();
        std::string readBuffer;
        
        if(curl) {
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, ("X-CMC_PRO_API_KEY: " + apiKey).c_str());
            
            curl_easy_setopt(curl, CURLOPT_URL, "https://pro-api.coinmarketcap.com/v1/cryptocurrency/listings/latest?limit=20");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            
            CURLcode res = curl_easy_perform(curl);
            
            if(res == CURLE_OK) {
                Json::Value root;
                Json::Reader reader;
                if(reader.parse(readBuffer, root) && root.isMember("data")) {
                    for(const auto& coin : root["data"]) {
                        if(coin.isMember("symbol")) {
                            symbols.push_back(coin["symbol"].asString() + "USDT");
                        }
                    }
                }
            }
            
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        return symbols;
    }
};

class FundingRateFetcher {
private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    std::pair<bool, std::vector<std::pair<double, std::string>>> getFundingRate(
        const std::string& symbol, 
        const std::string& startTime = "", 
        const std::string& endTime = "") {
        
        std::vector<std::pair<double, std::string>> results;
        std::string readBuffer;
        
        CURL* curl = curl_easy_init();
        
        if(curl) {
            std::string url = "https://api.bybit.com/v5/market/funding/history?category=linear&symbol=" + symbol;
            if (!startTime.empty()) {
                url += "&startTime=" + startTime;
            }
            if (!endTime.empty()) {
                url += "&endTime=" + endTime;
            }
            
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            
            CURLcode res = curl_easy_perform(curl);
            
            if(res != CURLE_OK) {
                std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
                curl_easy_cleanup(curl);
                return {false, results};
            }
            
            // 解析JSON響應
            Json::Value root;
            Json::Reader reader;
            bool parsingSuccessful = reader.parse(readBuffer, root);
            
            if (!parsingSuccessful) {
                std::cerr << "JSON解析失敗: " << reader.getFormattedErrorMessages() << std::endl;
                return {false, results};
            }

            if (!root.isMember("result") || !root["result"].isMember("list")) {
                std::cerr << "無效的JSON響應格式" << std::endl;
                return {false, results};
            }

            const Json::Value& list = root["result"]["list"];
            for (const Json::Value& item : list) {
                if (item.isMember("fundingRate") && item.isMember("fundingRateTimestamp")) {
                    results.push_back({
                        std::stod(item["fundingRate"].asString()),
                        item["fundingRateTimestamp"].asString()
                    });
                }
            }

            // 按資金費率降序排序
            std::sort(results.begin(), results.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

            // 只保留前20個結果
            if (results.size() > 20) {
                results.resize(20);
            }

            curl_easy_cleanup(curl);
            return {true, results};
        }
        return {false, results};
    }
};

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    curl_global_init(CURL_GLOBAL_ALL);
    
    CoinMarketCapAPI cmcApi;
    FundingRateFetcher fetcher;
    
    // 获取前20名加密货币
    auto topCoins = cmcApi.getTop20Cryptocurrencies();
    
    // 存储所有币种的资金费率
    std::map<std::string, std::vector<std::pair<double, std::string>>> allRates;
    
    // 获取每个币种的资金费率
    for(const auto& symbol : topCoins) {
        auto [success, rates] = fetcher.getFundingRate(symbol);
        if(success && !rates.empty()) {
            allRates[symbol] = rates;
        }
    }
    
    // 输出结果
    std::cout << "前10个加密货币的资金费率：" << std::endl;
    int count = 0;
    for(const auto& [symbol, rates] : allRates) {
        if(count >= 10) break;
        
        std::cout << "\n币对: " << symbol << std::endl;
        for(const auto& [rate, timestamp] : rates) {
            double annualizedRate = rate * (365 * 3) * 100;
            std::cout << "资金费率: " << rate << "% (年化: " << annualizedRate 
                      << "%), 时间: " << timestamp << std::endl;
        }
        count++;
    }
    
    curl_global_cleanup();
    return 0;
}
