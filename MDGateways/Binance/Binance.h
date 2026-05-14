#include <ranges>
#include <optional>
#include <PlatformComm/PlatformComm.h>
#include "WebSockets.h"
#include "RestApi.h"

static const std::string EXCHANGE_ID = "BINANCE"; 

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

struct BinanceConfigTag : StringEnum<MetaEnum::ConfigTags, BinanceConfigTag>
{
  static BinanceConfigTag const &wsHost()
  {
    static BinanceConfigTag instance{"wsHost"};
    return instance;
  }

  static BinanceConfigTag const &wsPort()
  {
    static BinanceConfigTag instance{"wsPort"};
    return instance;
  }

  static BinanceConfigTag const &wsPath()
  {
    static BinanceConfigTag instance{"wsPath"};
    return instance;
  }

  static BinanceConfigTag const &restHost()
  {
    static BinanceConfigTag instance{"restHost"};
    return instance;
  }

  static BinanceConfigTag const &restPort()
  {
    static BinanceConfigTag instance{"restPort"};
    return instance;
  }

  static BinanceConfigTag const &restApiVersion()
  {
    static BinanceConfigTag instance{"restApiVersion"}; 
    return instance;
  }

  static BinanceConfigTag const &wsThrottleRatePerSec()
  {
    static BinanceConfigTag instance{"wsThrottleRatePerSec"}; 
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
        static BinanceTag instance{"q"};
        return instance;
    }

    static BinanceTag const& symbol_websocket()
    { 
        static BinanceTag instance{"s"};
        return instance;
    }

    static BinanceTag const& symbol_restapi()
    { 
        static BinanceTag instance{"symbol"};
        return instance;
    }

    static BinanceTag const& bids()
    { 
        static BinanceTag instance{"bids"};
        return instance;
    }

    static BinanceTag const& asks()
    { 
        static BinanceTag instance{"asks"};
        return instance;
    }

    static BinanceTag const& data()
    { 
        static BinanceTag instance{"data"};
        return instance;
    }

    static BinanceTag const& stream()
    { 
        static BinanceTag instance{"stream"};
        return instance;
    }

    static BinanceTag const& symbol_list()
    { 
        static BinanceTag instance{"symbols"};
        return instance;
    }

};

inline std::optional<BinancePriceType> strToBinancePriceType(const std::string& priceType)
{
    if (priceType == *BinancePriceType::trade()) return BinancePriceType::trade();
    else if (priceType == *BinancePriceType::depthUpdate()) return BinancePriceType::depthUpdate();
    else return std::nullopt;
}

inline std::optional<PriceType> binanceToPlatformPriceType(const BinancePriceType& priceType)
{
    if (priceType == BinancePriceType::trade()) return PriceType::trade();
    else if (priceType == BinancePriceType::depthUpdate()) return PriceType::depth();
    else return std::nullopt;
}