#include "Binance.h"
#include "Constants.h"
#include "PlatformComm/PlatformComm.h"
#include <MDGatewayRouterConfig.h>
#include <Logging.h>
#include <ConfigLib/ConfigLib.h>
#include <NanoLogCpp17.h>
#include <boost/json/object.hpp>
#include <string>

using namespace NanoLog::LogLevels;

using Worker = ULMTTools::WorkerThread;
using Worker_SPtr = std::shared_ptr<Worker>;

using Scheduler = ULMTTools::TaskScheduler;
using Scheduler_SPtr = std::shared_ptr<Scheduler>;

using Timer = ULMTTools::Timer;
using Timer_SPtr = std::shared_ptr<Timer>;
SymbolStore symbolStore;

void initMiddleware(const std::shared_ptr<MDRoutingMethods>& routing,
        const KeyGenFunc& keyGenFunc,
        const std::string& brokers,
        const Timer_SPtr& timer,
        const Worker_SPtr& workerThread,
        const Middleware::ErrCallback& initErrorCb,
        const json::object& cfg)
{
    const std::string appId = cfg.at(*ConfigTag::appId()).as_string().c_str();
    const std::string appGroup = cfg.at(*ConfigTag::group()).as_string().c_str();
    const std::string inTopic = cfg.at(*ConfigTag::md_subscription_topic()).as_string().c_str();

    PlatformComm::init(routing,
        keyGenFunc,
        brokers,
        timer,
        workerThread,
        appId,
        appGroup,
        inTopic,
        initErrorCb,
        cfg.at(*ConfigTag::numMinBrokers()).as_int64());
}

bool addKey(json::object& update)
{
    try
    {
        const std::string priceType = update[*BinanceTag::priceType()].as_string().c_str();
        auto const& binancePriceType = strToBinancePriceType(priceType);
        if(!binancePriceType)
        {
            NANO_LOG(DEBUG, "Invalid priceType from exchange: %s", priceType.c_str());
            return false;
        }

        auto const& platformPriceType = binanceToPlatformPriceType(*binancePriceType);
        if(!platformPriceType)
        {
            NANO_LOG(DEBUG, "Unhandled binance pricetype: %s", (*(binancePriceType.value())).c_str());
            return false;
        }

        std::string instrument = update[*BinanceTag::symbol_websocket()].as_string().c_str();
        std::string key = instrument + *(platformPriceType.value());

        update[*Tags::subscriptionKey()] = key;
    }
    catch (const std::exception& ex)
    {
        NANO_LOG(DEBUG, "Problem while creating key from market update: %s", ex.what());
        return false;
    }

    return true;
}

bool transformForPlatform(json::object& update)
{
    if(!update.contains("s") || !update.contains("p") || !update.contains("q"))
    {
        // Log some error here about unexpected message format
        return false;
    }

    update["symbol"] = update["s"].as_string();
    update["price"] = update["p"].as_string();
    update["quantity"] = update["q"].as_string();
    
    update.erase("s");
    update.erase("p");
    update.erase("q");
    return true;
}

std::optional<std::string> generateKeyFromSubUnsubRequest(const json::object& subUnsubRequest)
{
    try{
        const std::string key = 
            std::string(subUnsubRequest.at(*Tags::symbol()).as_string().c_str()) +=
            std::string(":") +=
            subUnsubRequest.at(*Tags::subscription_type()).as_string();
        return key;
    }catch(const std::exception& ex) {
        NANO_LOG(DEBUG, "Exception while generating key from subscription request, details: %s", ex.what());
        return std::nullopt;
    }
}

void subscribe(const std::shared_ptr<session>& sess,
    const std::string& key)
{
    auto tokens = key |
                 std::views::split(':') | 
                 std::ranges::to<std::vector<std::string>>();

    if (tokens.size() < 2)
    {
        NANO_LOG(DEBUG, "Invalid key format for key: %s", key.c_str());
    }


    std::string& symbol = tokens[0];
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::tolower);
    if(auto priceType = strToPriceType(tokens[1]); priceType)
    {
        *priceType == PriceType::trade() ?
            sess->subscribeTrade(symbol) :
            sess->subscribeDepth(symbol);
    }    
}

void transformSnapshot(boost::json::object& obj, const std::string& symbol)
{
    json::value data = obj[*BinanceTag::data()];
    if (data.is_array())// Array comes for trade
    {
        obj[*BinanceTag::data()] = (data.as_array()[0]).as_object();
        auto& matter = obj[*BinanceTag::data()].as_object();
        matter["quantity"] = matter["qty"];
    }
    
    // "symbol" tag is requred regardless it's depth or trade
    auto& matter = obj[*BinanceTag::data()].as_object();
    matter[*Tags::symbol()] = symbol;
}

void getSnapshot(const std::shared_ptr<BinanceRestClient>& client,
    const std::string& symbolUnformatted,
    const PriceType& priceType,
    const SnapshotCallback& snapshotCallback)
{
    std::string symbol = symbolUnformatted;
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
    auto depthHandler =
    [snapshotCallback, symbol]
    (const boost::json::object& obj, const beast::error_code& ec){
        if (ec) snapshotCallback(obj, ec.message());
        else
        {
            boost::json::object copy{obj};
            json::object& depth = copy["data"].as_object();
            depth[*Tags::symbol()] = symbol;
            snapshotCallback(copy["data"].as_object(), "");
        }
    };

    auto tradeHandler =
    [snapshotCallback, symbol]
    (const boost::json::object& obj, const beast::error_code& ec){
        if (ec) snapshotCallback(obj, ec.message());
        else
        {
            json::object copy{obj};
            json::array& arr = copy["data"].as_array();
            json::object& trade = arr[0].as_object();
            trade[*Tags::symbol()] = symbol;
            trade["quantity"] = trade["qty"];
            trade.erase("qty");
            snapshotCallback(trade, "");
        }
    };


    if (priceType == *PriceType::depth())
    {
        client->getDepth(symbol, 5, depthHandler);
    }
    else if (priceType == *PriceType::trade())
    {
        client->getRecentTrades(symbol, 1, tradeHandler);
    }
}

void getInstrumentList(const std::shared_ptr<BinanceRestClient>& client,
    const IntrumentListCallback& intrumentListCallback)
{
    intrumentListCallback(symbolStore);
}

void unsubscribe(const std::shared_ptr<session>& sess,
    const std::string& key)
{
    auto tokens = key |
                 std::views::split(':') | 
                 std::ranges::to<std::vector<std::string>>();

    if (tokens.size() < 2)
    {
        NANO_LOG(DEBUG, "Invalid key format for key: %s", key.c_str());
    }


    std::string& symbol = tokens[0];
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::tolower);
    if(auto priceType = strToPriceType(tokens[1]); priceType)
    {
        *priceType == PriceType::trade() ?
            sess->unsubscribeTrade(symbol) :
            sess->unsubscribeDepth(symbol);
    }    
}

void onGatewayConnected(const std::shared_ptr<session>& sess,
    const std::shared_ptr<BinanceRestClient>& client,
    net::strand<net::io_context::executor_type>& strand,
    const Timer_SPtr& timer,
    const Worker_SPtr& workerThread,
    const std::shared_ptr<MDRoutingMethods>& routing,
    const json::object& cfg)
{
    const std::string brokers = cfg.at(*ConfigTag::brokers()).as_string().c_str();

    static const uint64_t kBinanceId = 1;

    // Register as responders for each request type
    routing->reqResMethods.registerAsResponder(kBinanceId, MDReqType::TradeSnapshot,
        [client](const MDReqVariant& data, const MDResponseHandler& handler) {
            const TradeSnapshotRequest& req = std::get<TradeSnapshotRequest>(data);
            getSnapshot(client, *req, PriceType::trade(),
                [handler](const boost::json::object& snapshot, const std::string& err) {
                    if (!err.empty()) return;
                    handler(true, MDRespVariant{TradeSnapshot{json::serialize(snapshot)}});
                }
            );
        }
    );

    routing->reqResMethods.registerAsResponder(kBinanceId, MDReqType::DepthSnapshot,
        [client](const MDReqVariant& data, const MDResponseHandler& handler) {
            const DepthSnapshotRequest& req = std::get<DepthSnapshotRequest>(data);
            getSnapshot(client, *req, PriceType::depth(),
                [handler](const boost::json::object& snapshot, const std::string& err) {
                    if (!err.empty()) return;
                    handler(true, MDRespVariant{DepthSnapshot{json::serialize(snapshot)}});
                }
            );
        }
    );

    routing->reqResMethods.registerAsResponder(kBinanceId, MDReqType::InstrumentList,
        [client](const MDReqVariant&, const MDResponseHandler& handler) {
            getInstrumentList(client, [handler](const SymbolStore& symbolStore) {
                NANO_LOG(NOTICE, "Providing instrument list, total %d instruments", (int)symbolStore.size());
                for (auto const& [_, symbolDetails] : symbolStore)
                {
                    handler(false, MDRespVariant{InstrumentRecord{symbolDetails}});
                }
                handler(true, MDRespVariant{InstrumentRecord{""}});
            });
        }
    );

    // Subscribe to ALL sub/unsub commands from PlatformComm
    routing->pubSubMethods.consumeAllControl(kBinanceId,
        [sess, &strand](const std::string& key, const SubUnsubVariant& cmd) {
            std::visit(overload{
                [&](const SubRequest&) {
                    net::post(strand, [sess, key]() {
                        NANO_LOG(DEBUG, "Subscribing to %s", key.c_str());
                        subscribe(sess, key);
                    });
                },
                [&](const UnsubRequest&) {
                    net::post(strand, [sess, key]() {
                        NANO_LOG(DEBUG, "Unsubscribing from %s", key.c_str());
                        unsubscribe(sess, key);
                    });
                }
            }, cmd);
        }
    );

    KeyGenFunc keyGenFunc = [](const std::variant<MDUpdateVariant, SubUnsubVariant>& v) -> std::optional<std::string> {
        return std::visit(overload{
            [](const MDUpdateVariant&) -> std::optional<std::string> { return std::nullopt; },
            [](const SubUnsubVariant& suv) -> std::optional<std::string> {
                return std::visit(overload{
                    [](const SubRequest& req)   { return generateKeyFromSubUnsubRequest(*req); },
                    [](const UnsubRequest& req) { return generateKeyFromSubUnsubRequest(*req); }
                }, suv);
            }
        }, v);
    };

    initMiddleware(routing, keyGenFunc, brokers, timer, workerThread,
        [](const Middleware::Error& error){
            NANO_LOG(DEBUG, "Error initializing middleware: %s", error.message().c_str());
        },
        cfg);
}

bool transformDepthForPlatForm(const std::string& symbol, json::object& obj)
{
    obj[*Tags::symbol()] = symbol;
    return true;
}

bool transformTradeForPlatForm(const std::string& symbol, json::object& obj)
{
    if (!obj.contains(*BinanceTag::symbol_websocket()) 
        || !obj.contains(*BinanceTag::price())
        || !obj.contains(*BinanceTag::quantity()))
    {
        NANO_LOG(DEBUG, "Invalid format for trade");
        return false;
    }
    obj[*Tags::symbol()] = obj[*BinanceTag::symbol_websocket()];
    obj[*Tags::price()] = obj[*BinanceTag::price()];
    obj[*Tags::quantity()] = obj[*BinanceTag::quantity()];

    obj.erase(*BinanceTag::symbol_websocket());
    obj.erase(*BinanceTag::price());
    obj.erase(*BinanceTag::quantity());

    return true;
}

std::optional<std::string> generateKeyFromMarketUpdate(const PriceType& priceType, const json::object& update)
{
    try
    {
        std::string instrument = update.at(*BinanceTag::symbol_websocket()).as_string().c_str();
        std::string key = instrument + ":" + *priceType;
        return key;
    }
    catch (const std::exception& ex)
    {
        NANO_LOG(DEBUG, "Problem while creating key from market update: %s", ex.what());
        return std::nullopt;
    }
}

void onPriceUpdate(const std::shared_ptr<MDRoutingMethods>& routing, const std::string& update)
{
    auto obj = json::parse(update).as_object();
    
    if(!obj.contains(*BinanceTag::data()))
    {
        NANO_LOG(DEBUG, "data tag missing");
        return;
    }
    
    
    const std::string& symbolAndstream = obj[*BinanceTag::stream()].as_string().c_str();
    auto tokens = symbolAndstream
            | std::views::split('@')
            | std::ranges::to<std::vector<std::string>>();
    
    if(tokens.size() < 2)
    {
        NANO_LOG(DEBUG, "Invalid value for stream: %s", symbolAndstream.c_str());
        return;
    }

    std::string& symbol = tokens[0];
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
    const std::string& stream = tokens[1];
    std::string key;

    obj = std::move(obj[*BinanceTag::data()].as_object());
    if (stream.find(*PriceType::trade()) != std::string::npos)
    {
        key = symbol + ":" + *PriceType::trade();
        if(!transformTradeForPlatForm(symbol, obj)) return;
        auto objStr = json::serialize(obj);
        routing->pubSubMethods.produceUpdate(key, MDUpdateVariant{TradeUpdate{std::move(objStr)}});
    }
    else if (stream.find(*PriceType::depth()) != std::string::npos)
    {
        key = symbol + ":" + *PriceType::depth();
        if(!transformDepthForPlatForm(symbol, obj)) return;
        auto objStr = json::serialize(obj);
        routing->pubSubMethods.produceUpdate(key, MDUpdateVariant{DepthUpdate{std::move(objStr)}});
    }
    else [[unlikely]]
    {
        NANO_LOG(DEBUG, "Invalid stream value: %s", stream.c_str());
        return;
    }
}

void launchWebSocketClient(net::io_context& ioc,
    ssl::context& ctx,
    net::strand<net::io_context::executor_type>& strand,
    const std::shared_ptr<BinanceRestClient>& client,
    const json::object& cfg)
{
    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto updateRouter  = std::make_shared<MDUpdateRouter>();
    auto controlRouter = std::make_shared<MDControlRouter>();
    auto reqResRouter  = std::make_shared<MDReqResRouter>();
    auto routing = std::make_shared<MDRoutingMethods>(updateRouter, controlRouter, reqResRouter);

    auto sessHolder = std::make_shared<std::shared_ptr<session>>();

    *sessHolder = std::make_shared<session>(strand,
        ctx,
        [routing](const std::string& update) { onPriceUpdate(routing, update); },
        host,
        port,
        path,
        10,
        [sessHolder, client, &strand, &ioc, &cfg, routing](const beast::error_code& ec){
            if (ec){ ioc.stop(); return;}

            auto sess = *sessHolder;
            std::thread([sess, &strand, client, &cfg, routing](){
                onGatewayConnected(sess,
                    client,
                    strand,
                    std::make_shared<Timer>(std::make_shared<Scheduler>()),
                    std::make_shared<Worker>(),
                    routing,
                    cfg);
            }).detach();
        }
    );
    (*sessHolder)->run();
}

void launchRestClient(net::io_context& ioc,
    ssl::context& ctx,
    net::strand<net::io_context::executor_type>& strand,
    const json::object& cfg)
{
    std::shared_ptr<BinanceRestClient> client =
    std::make_shared<BinanceRestClient>(strand,
        ctx,
        [&ioc, &ctx, &strand, &client, &cfg]
        (const beast::error_code& ec){
            if(ec)
            {
                NANO_LOG(DEBUG, "Error in Rest client init phase: %s", ec.message().c_str());
                ioc.stop();
                return;
            }
            NANO_LOG(DEBUG, "Fetching instrument list");
            client->getTradableInstruments(
                [&ioc, &ctx, &strand, &client, &cfg]
                (const json::object& obj, const beast::error_code& ec)
                {
                    if(ec)
                    {
                        NANO_LOG(ERROR, "Error whlie fetching instrument list, details: %s", ec.message().c_str());
                        ioc.stop();
                        return;
                    }
                    NANO_LOG(DEBUG, "Received instrument list");
                    auto const& symbols = obj.at(*BinanceTag::symbol_list()).as_array();
                    NANO_LOG(DEBUG, "Received symbol list with %d instruments from exchange", (int)symbols.size());
                    for(auto const& symbol : symbols)
                    {
                        //auto const& str = json::serialize(symbol);
                        //NANO_LOG(DEBUG, "Symbol info received: %s", str.c_str());
                        const std::string id = symbol.as_object().at(*BinanceTag::symbol_restapi()).as_string().c_str();
                        symbolStore[id] = json::serialize(symbol);
                    }

                    NANO_LOG(ERROR, "Launching WebSocker client");
                    launchWebSocketClient(ioc, ctx, strand, client, cfg);
            });
        },
        10
    );
    
    client->run();
    ioc.run();
}

bool validateConfig(const json::object& cfg)
{
    return true;
}

void onConfigUpdate(const json::object& cfg)
{
    const std::string logLevelStr = cfg.at(*ConfigTag::logLevel()).as_string().c_str();
    if (auto const& level_opt = strToLogLevel(logLevelStr); level_opt)
    {
        auto const& level = *level_opt;
        Logging::setLoggingLevel(level);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <appId>" << std::endl;
        return 1;
    }

    const std::string appId = argv[1];
    auto const& cfg_opt = Config::init(appId, onConfigUpdate, validateConfig);
    if (!cfg_opt)
    {
        return 1;
    }

    auto const& cfg = *cfg_opt;
    const std::string logLevelStr = cfg.at(*ConfigTag::logLevel()).as_string().c_str();
    auto logLevel_opt = strToLogLevel(logLevelStr);
    if (!logLevel_opt)
    {
        std::cout << "Invalid log level: " << logLevelStr
                  << ", should be one of ERROR, WARNING, INFO, DEBUG, case insensitive" << std::endl;
    }

    auto const& logLevel = *logLevel_opt;
    Logging::init(appId, logLevel);

    if (argc < 2)
    {
        NANO_LOG(DEBUG, "Usage: %s <available_brokers>", argv[0]);
        return 1;
    }

    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto workerThread = std::make_shared<Worker>();
    auto timer = std::make_shared<Timer>(std::make_shared<Scheduler>());

    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    net::strand<net::io_context::executor_type> strand{ioc.get_executor()};

    launchRestClient(ioc, ctx, strand, cfg);

    return 0;
}

