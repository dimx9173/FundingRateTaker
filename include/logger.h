#pragma once

#include <iostream>
#include <string>

class Logger {
public:
    //debug
    void debug(const std::string& message) {
        std::cout << "[DEBUG] " << message << std::endl;
    }

    //info
    void info(const std::string& message) {
        std::cout << "[INFO] " << message << std::endl;
    }
    
    //error
    void error(const std::string& message) {
        std::cerr << "[ERROR] " << message << std::endl;
    }
    
    //warning
    void warning(const std::string& message) {
        std::cerr << "[WARNING] " << message << std::endl;
    }
}; 