#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "trading/trading_module.h"
#include "exchange/exchange_interface.h"

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
};

class TradingModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        mockExchange = new MockExchange();
        ON_CALL(*mockExchange, getFundingRates())
            .WillByDefault(::testing::Return(std::vector<std::pair<std::string, double>>()));
        ON_CALL(*mockExchange, getTotalEquity())
            .WillByDefault(::testing::Return(10000.0));
    }

    void TearDown() override {
        delete mockExchange;
    }

    MockExchange* mockExchange;
};

TEST_F(TradingModuleTest, GetTopFundingRates) {
    std::vector<std::pair<std::string, double>> mockRates = {
        {"BTCUSDT", 0.001},
        {"ETHUSDT", 0.002}
    };
    
    EXPECT_CALL(*mockExchange, getFundingRates())
        .WillOnce(::testing::Return(mockRates));

    auto& trader = TradingModule::getInstance(*mockExchange);
    auto rates = trader.getTopFundingRates();
    
    ASSERT_EQ(rates.size(), 2);
    EXPECT_EQ(rates[0].first, "BTCUSDT");
    EXPECT_DOUBLE_EQ(rates[0].second, 0.001);
}
  