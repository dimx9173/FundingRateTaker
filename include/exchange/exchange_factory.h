#ifndef EXCHANGE_FACTORY_H
#define EXCHANGE_FACTORY_H

#include "exchange_interface.h"
#include "bybit_api.h"
#include "config.h"
#include <stdexcept>

class ExchangeFactory {
public:
    static IExchange& createExchange() {
        const auto& config = Config::getInstance();
        std::string exchangeName = config.getPreferredExchange();
        
        if (exchangeName == "BYBIT" && config.isExchangeEnabled("bybit")) {
            return BybitAPI::getInstance();
        }
        // 之後可以添加其他交易所的支援
        // else if (exchangeName == "BINANCE" && config.isExchangeEnabled("binance")) {
        //     return BinanceAPI::getInstance();
        // }
        
        throw std::runtime_error("不支援或未啟用的交易所: " + exchangeName);
    }
};

#endif // EXCHANGE_FACTORY_H 