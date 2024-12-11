# 編譯器設置
CXX = clang++
CXXFLAGS = -std=gnu++20 -Wall -g -I/opt/homebrew/include -I/usr/local/include

# 包含目錄
INCLUDES = -I/opt/homebrew/include

# 庫目錄和庫文件
LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib
LIBS = -ljsoncpp -lcurl -lsqlite3 -lssl -lcrypto

# 目標文件
TARGET = funding_rate_fetcher

# 源文件
SOURCES = funding_rate_fetcher.cpp

# 目標文件
OBJECTS = $(SOURCES:.cpp=.o)

# 默認目標
all: $(TARGET)

# 鏈接規則
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $(TARGET)

# 編譯規則
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 清理規則
clean:
	rm -f $(OBJECTS) $(TARGET)

# 重新編譯
rebuild: clean all

.PHONY: all clean rebuild