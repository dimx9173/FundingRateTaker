#pragma once

#include <iostream>
#include <string>

class Logger {
public:
    void info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }
    
    void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
    
    void warning(const std::string& message) {
        std::cerr << "[WARNING] " << message << std::endl;
    }
}; 