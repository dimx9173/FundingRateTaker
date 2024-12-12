#ifndef EXCHANGE_FACTORY_H
#define EXCHANGE_FACTORY_H

#include "exchange_interface.h"
#include "bybit_api.h"

class ExchangeFactory {
public:
    static IExchange& createExchange(const std::string& exchangeName) {
        if (exchangeName == "BYBIT") {
            return BybitAPI::getInstance();
        }
        throw std::runtime_error("目前只支援 BYBIT 交易所");
    }
};

#endif // EXCHANGE_FACTORY_H 