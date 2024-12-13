#include <gtest/gtest.h>
#include "storage/sqlite_storage.h"
#include <filesystem>

class SQLiteStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 使用測試數據庫文件
        const std::string TEST_DB = "trading_test.db";
        if (std::filesystem::exists(TEST_DB)) {
            std::filesystem::remove(TEST_DB);
        }
        storage = &SQLiteStorage::getInstance();
    }

    void TearDown() override {
        // 清理測試數據庫文件
        const std::string TEST_DB = "trading_test.db";
        if (std::filesystem::exists(TEST_DB)) {
            std::filesystem::remove(TEST_DB);
        }
    }

    SQLiteStorage* storage;
};

TEST_F(SQLiteStorageTest, StoreAndRetrieveTradeData) {
    ASSERT_TRUE(storage != nullptr);
    storage->storeTradeData("BTCUSDT", 0.001);
    
    auto trades = storage->getActiveTradeGroups();
    ASSERT_FALSE(trades.empty());
}

TEST_F(SQLiteStorageTest, StoreTradeGroup) {
    ASSERT_TRUE(storage != nullptr);
    storage->storeTradeGroup("BYBIT", "BTCUSDT", "SPOT123", "FUT123", 10);
    
    auto groups = storage->getActiveTradeGroups();
    ASSERT_FALSE(groups.empty());
    EXPECT_TRUE(groups[0].find("BTCUSDT") != std::string::npos);
}

// 測試數據庫連接狀態
TEST_F(SQLiteStorageTest, DatabaseConnection) {
    ASSERT_TRUE(storage != nullptr);
    // 通過嘗試存儲和讀取數據來間接測試連接狀態
    storage->storeTradeData("BTCUSDT", 0.001);
    auto trades = storage->getActiveTradeGroups();
    EXPECT_FALSE(trades.empty());
} 