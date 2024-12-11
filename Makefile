# 編譯器設置
CXX = clang++
CXXFLAGS = -std=gnu++20 -Wall -g

# 包含目錄
INCLUDES = -I/opt/homebrew/include -I/usr/local/include -Iinclude

# 庫目錄和庫文件
LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib
LIBS = -ljsoncpp -lcurl -lsqlite3 -lssl -lcrypto

# 目標文件
TARGET = funding_rate_fetcher

# 源文件
SOURCES = funding_rate_fetcher.cpp \
          lib/exchange/bybit_api.cpp \
          lib/config.cpp

# 目標文件 (保持在當前目錄)
OBJECTS = $(notdir $(SOURCES:.cpp=.o))

# 默認目標
all: $(TARGET)

# 鏈接規則
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

# 編譯規則
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: lib/exchange/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: lib/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 清理規則
clean:
	rm -f $(OBJECTS) $(TARGET)

# 重新編譯
rebuild: clean all

.PHONY: all clean rebuild