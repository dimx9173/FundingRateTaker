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
};

class TradingModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::testing::FLAGS_gtest_death_test_style = "threadsafe";
        ::testing::GTEST_FLAG(print_time) = true;
        
        // 重置單例
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
    
    std::vector<std::pair<std::string, double>> mockRates = {
        {"ETHUSDT", 0.002},
        {"BTCUSDT", 0.001}
    };
    
    std::cout << "設置 mock 數據" << std::endl;
    EXPECT_CALL(*mockExchange, getFundingRates())
        .WillOnce(::testing::Return(mockRates));

    std::cout << "獲取 TradingModule 實例" << std::endl;
    auto& trader = TradingModule::getInstance(*mockExchange);
    
    std::cout << "調用 getTopFundingRates" << std::endl;
    auto rates = trader.getTopFundingRates();
    
    std::cout << "驗證結果" << std::endl;
    ASSERT_EQ(rates.size(), 2);
    EXPECT_EQ(rates[0].first, "ETHUSDT");
    EXPECT_DOUBLE_EQ(rates[0].second, 0.002);
}
  