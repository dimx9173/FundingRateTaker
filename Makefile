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
          lib/exchange/bybit_api.cpp \
          lib/config.cpp

# 目標文件 (放在 obj 目錄)
OBJECTS = $(addprefix $(OBJ_DIR)/, $(notdir $(SOURCES:.cpp=.o)))

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

$(OBJ_DIR)/%.o: lib/exchange/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: lib/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 清理規則
clean:
	rm -rf $(TARGET_DIR)

# 重新編譯
rebuild: clean all

.PHONY: all clean rebuild