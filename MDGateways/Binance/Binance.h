#include <ranges>
#include <optional>
#include <PlatformComm/PlatformComm.h>
#include "WebSockets.h"

struct BinancePriceType : StringEnum<MetaEnum::PriceType, BinancePriceType>
{
    static BinancePriceType const& trade()
    { 
        static BinancePriceType instance{"trade"};
        return instance;
    }

    static BinancePriceType const& depthUpdate()
    { 
        static BinancePriceType instance{"depthUpdate"};
        return instance;
    }
};

struct BinanceTag : StringEnum<MetaEnum::Tags, BinanceTag>
{
    static BinanceTag const& priceType()
    { 
        static BinanceTag instance{"e"};
        return instance;
    }

    static BinanceTag const& price()
    { 
        static BinanceTag instance{"p"};
        return instance;
    }

    static BinanceTag const& quantity()
    { 
        static BinanceTag instance{"y"};
        return instance;
    }

    static BinanceTag const& symbol()
    { 
        static BinanceTag instance{"s"};
        return instance;
    }

};

std::optional<BinancePriceType> strToBinancePriceType(const std::string& priceType)
{
    if (priceType == *BinancePriceType::trade()) return BinancePriceType::trade();
    else if (priceType == *BinancePriceType::depthUpdate()) return BinancePriceType::depthUpdate();
    else return std::nullopt;
}

std::optional<PriceType> binanceToPlatformPriceType(const BinancePriceType& priceType)
{
    if (priceType == BinancePriceType::trade()) return PriceType::trade();
    else if (priceType == BinancePriceType::depthUpdate()) return PriceType::depth();
    else return std::nullopt;
}