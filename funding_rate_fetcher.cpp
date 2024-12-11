#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <chrono>
#include <hiredis/hiredis.h>
#include <json/json.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <fstream>
#include <openssl/hmac.h>
#include <openssl/sha.h>

class Config {
private:
    Json::Value config;
    Json::Value pair_list;
    static Config* instance;

    Config() {
        std::ifstream config_file("config.json");
        if (!config_file.is_open()) {
            throw std::runtime_error("Unable to open config.json");
        }

        Json::Reader reader;
        if (!reader.parse(config_file, config)) {
            throw std::runtime_error("Failed to parse config.json");
        }

        std::ifstream pair_list_file("pair_list.json");
        if (!pair_list_file.is_open()) {
            throw std::runtime_error("Unable to open pair_list.json");
        }

        if (!reader.parse(pair_list_file, pair_list)) {
            throw std::runtime_error("Failed to parse pair_list.json");
        }
    }

public:
    static Config& getInstance() {
        if (instance == nullptr) {
            instance = new Config();
        }
        return *instance;
    }

    const std::string getBybitApiKey() const {
        return config["bybit"]["api_key"].asString();
    }

    const std::string getBybitApiSecret() const {
        return config["bybit"]["api_secret"].asString();
    }

    const std::string getBybitBaseUrl() const {
        return config["bybit"]["base_url"].asString();
    }

    int getDefaultLeverage() const {
        return config["bybit"]["default_leverage"].asInt();
    }

    double getMinQuantity() const {
        return config["trading"]["min_quantity"].asDouble();
    }

    int getCheckIntervalHours() const {
        return config["trading"]["check_interval_hours"].asInt();
    }

    std::vector<std::string> getTradingPairs() const {
        std::vector<std::string> pairs;
        const Json::Value& pairArray = pair_list["pair_list"];
        for (const auto& pair : pairArray) {
            pairs.push_back(pair.asString());
        }
        return pairs;
    }

    double getMinTradeAmount() const {
        return config["trading"]["min_trade_amount"].asDouble();
    }

    double getMaxTradeAmount() const {
        return config["trading"]["max_trade_amount"].asDouble();
    }

    int getMaxPositions() const {
        return config["trading"]["max_positions"].asInt();
    }

    double getStopLossPercentage() const {
        return config["trading"]["stop_loss_percentage"].asDouble();
    }

    ~Config() {
        // 清理工作
    }
};

Config* Config::instance = nullptr;

// API 模块
class BybitAPI {
private:
    const std::string API_KEY;
    const std::string API_SECRET;
    const std::string BASE_URL;
    
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string generateSignature(const std::string& params, const std::string& timestamp) {
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

    Json::Value makeRequest(const std::string& endpoint, const std::string& method, 
                          const std::map<std::string, std::string>& params = {}) {
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
            
            // 构建参数字符串
            if (method == "GET") {
                for (const auto& [key, value] : params) {
                    if (!paramString.empty()) paramString += "&";
                    paramString += key + "=" + value;
                }
                if (!paramString.empty()) {
                    url += "?" + paramString;
                }
            } else if (method == "POST") {
                // POST 请求使用 JSON 格式
                Json::Value jsonParams;
                for (const auto& [key, value] : params) {
                    jsonParams[key] = value;
                }
                Json::FastWriter writer;
                paramString = writer.write(jsonParams);
                // 移除末尾的换行符
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
            
            std::cout << "URL: " << url << std::endl;
            std::cout << "Timestamp: " << timestamp << std::endl;
            std::cout << "Signature: " << signature << std::endl;
            if (method == "POST") {
                std::cout << "POST Data: " << paramString << std::endl;
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
                std::cout << "API错误: " << root["retMsg"].asString() << std::endl;
                std::cout << "请求URL: " << url << std::endl;
                std::cout << "参数: " << paramString << std::endl;
            }
            return root;
        } else {
            std::cerr << "解析响应失败: " << response << std::endl;
            return Json::Value();
        }
    }

public:
    BybitAPI() : 
        API_KEY(Config::getInstance().getBybitApiKey()),
        API_SECRET(Config::getInstance().getBybitApiSecret()),
        BASE_URL(Config::getInstance().getBybitBaseUrl()) {
    }

    std::vector<std::pair<std::string, double>> getFundingRates() {
        std::vector<std::pair<std::string, double>> rates;
        std::vector<std::string> tradingPairs = Config::getInstance().getTradingPairs();
        
        for (const auto& pair : tradingPairs) {
            std::map<std::string, std::string> params = {
                {"category", "linear"},
                {"symbol", pair},
                {"limit", "1"}  // 只获取最新的一条记录
            };
            
            Json::Value response = makeRequest("/v5/market/funding/history", "GET", params);
            
            if (response["retCode"].asInt() == 0) {
                const Json::Value& list = response["result"]["list"];
                if (!list.empty()) {
                    std::string symbol = list[0]["symbol"].asString();
                    double rate = std::stod(list[0]["fundingRate"].asString());
                    rates.emplace_back(symbol, rate);
                }
            }
        }
        return rates;
    }

    bool setLeverage(const std::string& symbol, int leverage) {
        // 验证杠杆范围
        if (leverage < 1 || leverage > 100) {  // 假设最大杠杆是100
            std::cout << "杠杆值无效，必须在1到100之间" << std::endl;
            return false;
        }

        std::string leverageStr = std::to_string(leverage);
        std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"buyLeverage", leverageStr},
            {"sellLeverage", leverageStr}
        };
        
        Json::Value response = makeRequest("/v5/position/set-leverage", "POST", params);
        if (response["retCode"].asInt() != 0) {
            std::cout << "设置杠杆失败: " << response["retMsg"].asString() << std::endl;
            std::cout << "错误代码: " << response["retCode"].asString() << std::endl;
            return false;
        }
        
        std::cout << "成功设置" << symbol << "的杠杆为" << leverage << "倍" << std::endl;
        return true;
    }

    // 统一的下单方法
    Json::Value createOrder(const std::string& symbol, 
                          const std::string& side, 
                          double qty,
                          const std::string& category = "linear",  // 默认为合约
                          const std::string& orderType = "Market") {
        std::map<std::string, std::string> params = {
            {"category", category},
            {"symbol", symbol},
            {"side", side},
            {"orderType", orderType},
            {"qty", std::to_string(qty)}
        };
        
        Json::Value response = makeRequest("/v5/order/create", "POST", params);
        if (response["retCode"].asInt() != 0) {
            std::cout << "下单失败: " << response["retMsg"].asString() << std::endl;
        }
        return response;
    }

    // 创建现货订单的便捷方法
    bool createSpotOrder(const std::string& symbol, const std::string& side, double qty) {
        Json::Value response = createOrder(symbol, side, qty, "spot");
        return response["retCode"].asInt() == 0;
    }

    void closePosition(const std::string& symbol) {
        std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol}
        };
        
        Json::Value positions = makeRequest("/v5/position/list", "GET", params);
        if (positions["retCode"].asInt() == 0 && !positions["result"]["list"].empty()) {
            const Json::Value& pos = positions["result"]["list"][0];
            std::string side = pos["side"].asString();
            double size = std::stod(pos["size"].asString());
            
            std::string closeSide = (side == "Buy") ? "Sell" : "Buy";
            createOrder(symbol, closeSide, size);  // 使用统一的下单方法
        }
    }

    void executeHedgeStrategy() {
        double totalEquity = getTotalEquity();
        if (totalEquity <= 0) {
            std::cout << "总资金为0，无法进行对冲交易" << std::endl;
            return;
        }

        double hedgeAmount = totalEquity / 10;
        std::vector<std::string> tradingPairs = Config::getInstance().getTradingPairs();

        for (const auto& pair : tradingPairs) {
            double spotQty = hedgeAmount / getSpotPrice(pair);
            double leverage = 2;
            double contractQty = spotQty * leverage;

            bool spotOrderSuccess = createSpotOrder(pair, "Buy", spotQty);
            Json::Value contractOrder = createOrder(pair, "Sell", contractQty);
            bool contractOrderSuccess = contractOrder["retCode"].asInt() == 0;

            if (!spotOrderSuccess || !contractOrderSuccess) {
                std::cout << "对冲交易失败，立即平仓" << std::endl;
                closePosition(pair);
            } else {
                std::cout << "成功对冲交易 " << pair << std::endl;
            }
        }
    }

    Json::Value getPositions(const std::string& symbol = "") {
        std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"settleCoin", "USDT"}
        };
        if (!symbol.empty()) {
            params["symbol"] = symbol;
        }
        
        return makeRequest("/v5/position/list", "GET", params);
    }

    // 获取钱包余额
    void getWalletBalance() {
        std::map<std::string, std::string> params = {
            {"accountType", "UNIFIED"},
            {"coin", "USDT"}
        };
        
        Json::Value response = makeRequest("/v5/account/wallet-balance", "GET", params);
        if (response["retCode"].asInt() == 0 && !response["result"]["list"].empty()) {
            const Json::Value& balance = response["result"]["list"][0];
            std::cout << "\n=== 钱包余额 ===" << std::endl;
            std::cout << "总额: " << balance["totalEquity"].asString() << " USDT" << std::endl;
            std::cout << "可用余额: " << balance["availableBalance"].asString() << " USDT" << std::endl;
            std::cout << "已用保证金: " << balance["usedMargin"].asString() << " USDT" << std::endl;
            std::cout << "未实现盈亏: " << balance["unrealisedPnl"].asString() << " USDT" << std::endl;
        }
    }

    // 获取并显示当前持仓
    void displayPositions() {
        Json::Value response = getPositions();
        if (response["retCode"].asInt() == 0) {
            const Json::Value& positions = response["result"]["list"];
            
            std::cout << "\n=== 当前持仓 ===" << std::endl;
            if (positions.size() == 0) {
                std::cout << "当前没有持仓" << std::endl;
                return;
            }

            for (const auto& pos : positions) {
                std::cout << "\n币对: " << pos["symbol"].asString() << std::endl;
                std::cout << "方向: " << pos["side"].asString() << std::endl;
                std::cout << "数量: " << pos["size"].asString() << std::endl;
                std::cout << "入场价格: " << pos["entryPrice"].asString() << std::endl;
                std::cout << "未实现盈亏: " << pos["unrealisedPnl"].asString() << std::endl;
                std::cout << "杠杆: " << pos["leverage"].asString() << "x" << std::endl;
            }
        } else {
            std::cout << "获取持仓信息失败: " << response["retMsg"].asString() << std::endl;
        }
    }

    double getTotalEquity() {
        std::map<std::string, std::string> params = {
            {"accountType", "UNIFIED"},
            {"coin", "USDT"}
        };
        
        Json::Value response = makeRequest("/v5/account/wallet-balance", "GET", params);
        if (response["retCode"].asInt() == 0 && !response["result"]["list"].empty()) {
            return std::stod(response["result"]["list"][0]["totalEquity"].asString());
        }
        return 0.0;
    }

    double getSpotPrice(const std::string& symbol) {
        std::map<std::string, std::string> params = {
            {"category", "spot"},
            {"symbol", symbol}
        };
        
        Json::Value response = makeRequest("/v5/market/tickers", "GET", params);
        if (response["retCode"].asInt() == 0 && !response["result"]["list"].empty()) {
            return std::stod(response["result"]["list"][0]["lastPrice"].asString());
        }
        return 0.0;
    }
};

// 资金费率获取模块
class FundingRateFetcher {
private:
    BybitAPI api;
public:
    std::vector<std::pair<std::string, double>> getTop10FundingRates() {
        auto rates = api.getFundingRates();
        std::sort(rates.begin(), rates.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (rates.size() > 10) rates.resize(10);
        return rates;
    }
};

// 数据存储模块
class SQLiteStorage {
private:
    sqlite3* db;
    bool isConnected;

    void initTables() {
        const char* sql[] = {
            // 交易组表
            "CREATE TABLE IF NOT EXISTS trade_groups ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "exchange_id TEXT,"
            "symbol TEXT,"
            "spot_order_id TEXT,"
            "futures_order_id TEXT,"
            "leverage INTEGER,"
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");",
            
            // 资金费率历史
            "CREATE TABLE IF NOT EXISTS funding_rates ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "symbol TEXT,"
            "rate REAL,"
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        };
        
        for (const auto& query : sql) {
            char* errMsg = nullptr;
            int rc = sqlite3_exec(db, query, nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK) {
                std::cerr << "SQL error: " << errMsg << std::endl;
                sqlite3_free(errMsg);
            }
        }
    }

public:
    SQLiteStorage() : db(nullptr), isConnected(false) {
        int rc = sqlite3_open("trading.db", &db);
        if (rc) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        
        // 創建表格（如果不存在）
        const char* sql = "CREATE TABLE IF NOT EXISTS trades ("
                         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "symbol TEXT NOT NULL,"
                         "rate REAL NOT NULL,"
                         "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
                         ");";
        char* errMsg = nullptr;
        rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::cerr << "SQL error: " << errMsg << std::endl;
            sqlite3_free(errMsg);
            return;
        }
        
        isConnected = true;
    }

    ~SQLiteStorage() {
        if (db) {
            sqlite3_close(db);
        }
    }

    void storeTradeData(const std::string& symbol, const double rate) {
        if (!isConnected || !db) {
            std::cerr << "Cannot store data: SQLite not connected" << std::endl;
            return;
        }

        const char* sql = "INSERT INTO trades (symbol, rate) VALUES (?, ?);";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return;
        }

        sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 2, rate);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "Failed to insert data: " << sqlite3_errmsg(db) << std::endl;
        }

        sqlite3_finalize(stmt);
    }

    bool isConnectionValid() const {
        return isConnected && db != nullptr;
    }

    void storeTradeGroup(const std::string& exchangeId, const std::string& symbol,
                        const std::string& spotOrderId, const std::string& futuresOrderId,
                        int leverage) {
        if (!isConnected || !db) {
            std::cerr << "Cannot store trade group: SQLite not connected" << std::endl;
            return;
        }

        const char* sql = "INSERT INTO trade_groups (exchange_id, symbol, spot_order_id, "
                         "futures_order_id, leverage) VALUES (?, ?, ?, ?, ?);";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return;
        }

        sqlite3_bind_text(stmt, 1, exchangeId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, symbol.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, spotOrderId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, futuresOrderId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 5, leverage);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            std::cerr << "Failed to insert trade group: " << sqlite3_errmsg(db) << std::endl;
        }

        sqlite3_finalize(stmt);
    }
};

// 交易模块
class TradingModule {
private:
    BybitAPI api;
    SQLiteStorage& db;
    const int DEFAULT_LEVERAGE;
    const double MIN_TRADE_AMOUNT;
    const double MAX_TRADE_AMOUNT;
    const int MAX_POSITIONS;
    const double STOP_LOSS_PERCENTAGE;

    struct Position {
        std::string symbol;
        std::string side;
        double size;
        double entryPrice;
    };

    double calculateTradeAmount() {
        return MIN_TRADE_AMOUNT + 
               (rand() % static_cast<int>(MAX_TRADE_AMOUNT - MIN_TRADE_AMOUNT + 1));
    }

    bool shouldClosePosition(const Position& pos, double currentPrice) {
        if (pos.side == "Buy") {
            return (pos.entryPrice - currentPrice) / pos.entryPrice * 100 >= STOP_LOSS_PERCENTAGE;
        } else {
            return (currentPrice - pos.entryPrice) / pos.entryPrice * 100 >= STOP_LOSS_PERCENTAGE;
        }
    }

    std::string generateTradeGroupId(const std::string& symbol, 
                                   const std::string& spotOrderId, 
                                   const std::string& futuresOrderId) {
        return "BYBIT:" + symbol + ":" + spotOrderId + "_" + futuresOrderId + 
               "_" + std::to_string(DEFAULT_LEVERAGE);
    }

public:
    TradingModule(SQLiteStorage& storage) : 
        db(storage),
        DEFAULT_LEVERAGE(Config::getInstance().getDefaultLeverage()),
        MIN_TRADE_AMOUNT(Config::getInstance().getMinTradeAmount()),
        MAX_TRADE_AMOUNT(Config::getInstance().getMaxTradeAmount()),
        MAX_POSITIONS(Config::getInstance().getMaxPositions()),
        STOP_LOSS_PERCENTAGE(Config::getInstance().getStopLossPercentage()) {
    }

    void executeTrades(const std::vector<std::pair<std::string, double>>& topRates) {
        // 取当前持仓
        Json::Value currentPositions = api.getPositions();
        std::vector<Position> positions;
        
        // 解析当前持仓
        if (currentPositions["retCode"].asInt() == 0) {
            const Json::Value& list = currentPositions["result"]["list"];
            for (const auto& pos : list) {
                Position position {
                    pos["symbol"].asString(),
                    pos["side"].asString(),
                    std::stod(pos["size"].asString()),
                    std::stod(pos["entryPrice"].asString())
                };
                positions.push_back(position);
            }
        }

        // 检查并关闭不在前10的持仓
        for (const auto& pos : positions) {
            bool inTop10 = false;
            for (const auto& [symbol, rate] : topRates) {
                if (pos.symbol == symbol) {
                    inTop10 = true;
                    break;
                }
            }
            
            if (!inTop10) {
                // 平仓
                std::string closeSide = (pos.side == "Buy") ? "Sell" : "Buy";
                api.createOrder(pos.symbol, closeSide, pos.size);
            }
        }

        // 对前10进行开仓
        for (const auto& [symbol, rate] : topRates) {
            if (positions.size() >= MAX_POSITIONS) break;

            // 检查是否已有该币种持仓
            auto it = std::find_if(positions.begin(), positions.end(),
                [&symbol](const Position& pos) { return pos.symbol == symbol; });
            
            if (it == positions.end()) {
                // 设置杠杆
                if (!api.setLeverage(symbol, DEFAULT_LEVERAGE)) {
                    continue;
                }

                double tradeAmount = calculateTradeAmount();
                std::string side = rate > 0 ? "Sell" : "Buy";

                // 创建合约订单
                Json::Value futuresOrder = api.createOrder(symbol, side, tradeAmount);
                if (futuresOrder["retCode"].asInt() == 0) {
                    std::string futuresOrderId = futuresOrder["result"]["orderId"].asString();
                    
                    // 创建现货订单（反向）
                    std::string spotSide = (side == "Buy") ? "Sell" : "Buy";
                    Json::Value spotOrder = api.createOrder(symbol, spotSide, tradeAmount);
                    
                    if (spotOrder["retCode"].asInt() == 0) {
                        std::string spotOrderId = spotOrder["result"]["orderId"].asString();
                        
                        // 生成并存储交易ID
                        std::string tradeGroupId = generateTradeGroupId(symbol, spotOrderId, futuresOrderId);
                        db.storeTradeGroup("BYBIT", symbol, spotOrderId, futuresOrderId, DEFAULT_LEVERAGE);
                    }
                }
            }
        }
    }
};

// 调度器
void scheduleTask() {
    try {
        std::cout << "Starting scheduled task..." << std::endl;

        FundingRateFetcher fetcher;
        SQLiteStorage storage;
        TradingModule trader(storage);
        BybitAPI api;  // 创建 API 实例

        if (!storage.isConnectionValid()) {
            std::cerr << "Failed to connect to SQLite database, exiting..." << std::endl;
            return;
        }

        while (true) {
            // 显示当前资产和持仓状态
            api.getWalletBalance();
            api.displayPositions();
            
            std::cout << "\nFetching top 10 funding rates..." << std::endl;
            auto topRates = fetcher.getTop10FundingRates();
            
            // 执行交易
            trader.executeTrades(topRates);

            // 存储资金费率数据
            for (const auto& [symbol, rate] : topRates) {
                std::cout << "Storing trade data for symbol: " << symbol << " with rate: " << rate << std::endl;
                storage.storeTradeData(symbol, rate);
            }

            // 再次显示更新后的资产和持仓状态
            std::cout << "\n=== 交易后状态 ===" << std::endl;
            api.getWalletBalance();
            api.displayPositions();

            std::cout << "\nSleeping for " << Config::getInstance().getCheckIntervalHours() << " hours..." << std::endl;
            std::this_thread::sleep_for(
                std::chrono::hours(Config::getInstance().getCheckIntervalHours())
            );
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

int main() {
    scheduleTask();
    return 0;
}
