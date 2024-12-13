# 編譯器設置
CXX = clang++
CXXFLAGS = -std=gnu++20 -Wall -g

# 包含目錄
INCLUDES = -I/opt/homebrew/include -I/usr/local/include -Iinclude

# 庫目錄和庫文件
LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib
LIBS = -ljsoncpp -lcurl -lsqlite3 -lssl -lcrypto

# 目錄設置
TARGET_DIR = target
OBJ_DIR = $(TARGET_DIR)/obj

# 目標文件
TARGET = $(TARGET_DIR)/funding_rate_fetcher

# 源文件
SOURCES = funding_rate_fetcher.cpp \
          src/exchange/bybit_api.cpp \
          src/config.cpp \
          src/trading/trading_module.cpp \
          src/storage/sqlite_storage.cpp

# 目標文件 (放在 obj 目錄)
OBJECTS = $(addprefix $(OBJ_DIR)/, $(notdir $(SOURCES:.cpp=.o)))

# 測試相關設置
TEST_DIR = tests
TEST_TARGET = $(TARGET_DIR)/run_tests
TEST_SOURCES = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJECTS = $(addprefix $(OBJ_DIR)/, $(notdir $(TEST_SOURCES:.cpp=.o)))

# 添加 Google Test 庫
TEST_LIBS = -lgtest -lgtest_main -lgmock -lgmock_main

# 創建必要的目錄
$(shell mkdir -p $(TARGET_DIR) $(OBJ_DIR))

# 默認目標
all: $(TARGET)

# 鏈接規則
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

# 編譯規則
$(OBJ_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: src/exchange/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: src/trading/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: src/storage/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 測試目標
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(OBJECTS) $(TEST_OBJECTS)
	$(CXX) $^ $(LDFLAGS) $(LIBS) $(TEST_LIBS) -o $@

$(OBJ_DIR)/%.o: $(TEST_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 清理規則
clean:
	rm -rf $(TARGET_DIR)

# 重新編譯
rebuild: clean all

.PHONY: all clean rebuild test