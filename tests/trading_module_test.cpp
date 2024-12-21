#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "include/trading/trading_module.h"
#include "include/exchange/exchange_interface.h"

// Mock Exchange 類
class MockExchange : public IExchange {
public:
    MOCK_METHOD0(getFundingRates, std::vector<std::pair<std::string, double>>());
    MOCK_METHOD1(getSpotPrice, double(const std::string&));
    MOCK_METHOD0(getTotalEquity, double());
    MOCK_METHOD1(getPositions, Json::Value(const std::string&));
    MOCK_METHOD2(setLeverage, bool(const std::string&, int));
    MOCK_METHOD5(createOrder, Json::Value(
        const std::string&, 
        const std::string&, 
        double, 
        const std::string&, 
        const std::string&
    ));
    MOCK_METHOD3(createSpotOrder, bool(const std::string&, const std::string&, double));
    MOCK_METHOD1(closePosition, void(const std::string&));
    MOCK_METHOD1(getInstruments, std::vector<std::string>(const std::string&));
    MOCK_METHOD0(getLastError, std::string());
    

    // Mock 方法的具體實現
    MOCK_METHOD0(displayPositions, void());
    MOCK_METHOD0(getSpotBalances, Json::Value());
    MOCK_METHOD1(getSpotBalance, double(const std::string&));
    MOCK_METHOD1(getMarginRatio, double(const std::string&));
    MOCK_METHOD1(getSpotOrderBook, Json::Value(const std::string&));
    MOCK_METHOD1(getContractOrderBook, Json::Value(const std::string&));
    MOCK_METHOD1(getContractPrice, double(const std::string&));
    MOCK_METHOD1(getCurrentFundingRate, double(const std::string&));
    MOCK_METHOD0(getSpotFeeRate, double());
    MOCK_METHOD0(getContractFeeRate, double());
    
    MOCK_METHOD1(getFundingHistory, 
        std::vector<std::pair<std::string, std::vector<double>>>(const std::vector<std::string>&));
};

class TradingModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::testing::FLAGS_gtest_death_test_style = "threadsafe";
        ::testing::GTEST_FLAG(print_time) = true;
        
        TradingModule::resetInstance();
        
        mockExchange = new MockExchange();
        ON_CALL(*mockExchange, getFundingRates())
            .WillByDefault(::testing::Return(std::vector<std::pair<std::string, double>>()));
        ON_CALL(*mockExchange, getTotalEquity())
            .WillByDefault(::testing::Return(10000.0));
        ON_CALL(*mockExchange, getInstruments(::testing::_))
            .WillByDefault(::testing::Return(std::vector<std::string>{"BTCUSDT", "ETHUSDT"}));
        
    }

    void TearDown() override {
        delete mockExchange;
        TradingModule::resetInstance();
        std::remove("config.json");
    }

    MockExchange* mockExchange;
};

TEST_F(TradingModuleTest, GetTopFundingRates) {
    std::cout << "開始測試 GetTopFundingRates" << std::endl;
    
    // 設置歷史資金費率數據
    std::vector<std::pair<std::string, std::vector<double>>> mockHistoricalRates = {
        {"ETHUSDT", {0.002, 0.001, 0.003}},
        {"BTCUSDT", {0.001, 0.002, 0.001}}
    };
    
    auto& trader = TradingModule::getInstance(*mockExchange);
    auto rates = trader.getTopFundingRates();
    
    ASSERT_EQ(rates.size(), 2);
}
  