#include "Binance.h"

DataFunc dataFunc;
bool middlewareInitialized = false;

using Worker = ULMTTools::WorkerThread;
using Worker_SPtr = std::shared_ptr<Worker>;

using Scheduler = ULMTTools::TaskScheduler;
using Scheduler_SPtr = std::shared_ptr<Scheduler>;

using Timer = ULMTTools::Timer;
using Timer_SPtr = std::shared_ptr<Timer>;

uint16_t availableBrokers = 0;

void initMiddleware(const std::string& brokers,
        const SubUnsubFunc& subFunc,
        const SubUnsubFunc& unsububFunc,
        const GetPriceSnapshotFunc& getPriceSnapshotFunc,
        const KeyGenFunc& keyGenFunc,
        const std::function<void(const DataFunc&)>& registrationFunc,
        const Timer_SPtr& timer,
        const Worker_SPtr& workerThread,
        const std::string& appId,
        const std::string& appGroup,
        const std::string& inTopic,
        const Middleware::ErrCallback& initErrorCb)
{
    PlatformComm::init(brokers,
        subFunc,
        unsububFunc,
        getPriceSnapshotFunc,
        keyGenFunc,
        registrationFunc,
        timer,
        workerThread,
        appId,
        appGroup,
        inTopic,
        initErrorCb,
        availableBrokers);
}

bool addKey(json::object& update)
{
    try
    {
        const std::string priceType = update[*BinanceTag::priceType()].as_string().c_str();
        auto const& binancePriceType = strToBinancePriceType(priceType);
        if(!binancePriceType)
        {
            std::cout << "Innvalid priceType from exchange: " << priceType;
            return false;
        }

        auto const& platformPriceType = binanceToPlatformPriceType(*binancePriceType);
        if(!platformPriceType)
        {
            std::cout << "Unhandled binance pricetype: " << *(binancePriceType.value()) << std::endl;
            return false;
        }

        std::string instrument = update[*BinanceTag::symbol()].as_string().c_str();
        std::string key = instrument + *(platformPriceType.value());

        update[*Tags::subscriptionKey()] = key;
    }
    catch (const std::exception& ex)
    {
        std::cout << "Problem while creating key from market update: " << ex.what() << std::endl;
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
        std::cout << "Exception while generatig key from subscription request, details: " << ex.what() << std::endl;
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
        std::cout<< "Invalid key format for key: " << key << std::endl;
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
    json::value data = obj["data"];
    if (data.is_array())
    {
        obj["data"] = (data.as_array()[0]).as_object();
        auto& matter = obj["data"].as_object();
        matter["quantity"] = matter["qty"];
    }
    
    auto& matter = obj["data"].as_object();
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

void unsubscribe(const std::shared_ptr<session>& sess,
    const std::string& key)
{
    auto tokens = key |
                 std::views::split(':') | 
                 std::ranges::to<std::vector<std::string>>();

    if (tokens.size() < 2)
    {
        std::cout<< "Invalid key format for key: " << key << std::endl;
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
    const Worker_SPtr& workerThread)
{
    if (middlewareInitialized) return;
    middlewareInitialized = true;
    
    initMiddleware("127.0.0.1:9092",
        [&sess, &strand]
        (const std::string& key){
            net::post(strand, [&sess, key](){
                std::cout << "Subscribing to " << key << std::endl;
                subscribe(sess, key);
            });  
        },
        [&sess, &strand](const std::string& key){
            net::post(strand, [&sess, key](){
                    std::cout << "Unsubscribing from " << key << std::endl;
                    unsubscribe(sess, key);
            });
        },
        [client](const std::string& symbol,
            const PriceType& priceType,
            const SnapshotCallback& snapshotCallback)
        {
            getSnapshot(client, symbol, priceType, snapshotCallback);
        },
        generateKeyFromSubUnsubRequest,
        [](const DataFunc& dataFunc){
            // Registering the callback to receive price data from the exchange
            ::dataFunc = dataFunc;
        },
        timer,
        workerThread,
        "binance_price_fetcher_node_6",
        "binance_price_fetcher_6",
        "test_topic",
        [](const Middleware::Error& error){
            std::cerr << "Error initializing middleware: " << error.message() << "\n";
        });
}

std::optional<PriceType> generatePlatfromPriceTypeFromMarketUpdate(const json::object& update)
{
    try
    {
        const std::string priceType = update.at(*BinanceTag::priceType()).as_string().c_str();
        
        auto const& binancePriceType = strToBinancePriceType(priceType);
        if(!binancePriceType)
        {
            std::cout << "Innvalid priceType from exchange: " << priceType;
            return std::nullopt;
        }

        return binanceToPlatformPriceType(*binancePriceType);
    }
    catch (const std::exception& ex)
    {
        std::cout << "Problem while creating key from market update: " << ex.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<std::string> generateKeyFromMarketUpdate(const PriceType& priceType, const json::object& update)
{
    try
    {
        std::string instrument = update.at(*BinanceTag::symbol()).as_string().c_str();
        std::string key = instrument + ":" + *priceType;
        return key;
    }
    catch (const std::exception& ex)
    {
        std::cout << "Problem while creating key from market update: " << ex.what() << std::endl;
        return std::nullopt;
    }
}

void onPriceUpdate(const std::string& update)
{
    auto obj = json::parse(update).as_object();
    
    if(!obj.contains(*BinanceTag::data()))
    {
        std::cout << "data tag missing" << std::endl;
        return;
    }
    
    obj = std::move(obj[*BinanceTag::data()].as_object());
    auto priceType = generatePlatfromPriceTypeFromMarketUpdate(obj);
    if(!priceType)
    {
        std::cout << "Unable to generate the key for this update: " << update << std::endl;
        return;
    }

    auto key = generateKeyFromMarketUpdate(*priceType, obj);
    if(!key)
    {
        std::cout << "Unable to generate the key for this update: " << update << std::endl;
        return;
    }
    
    if (!transformForPlatform(obj)) 
    {
        std::cout << "Object transformation failed, update was: " << update << std::endl;
        return;
    }

    auto objStr = std::make_shared<std::string>(std::move(json::serialize(obj)));
    auto keyPtr = std::make_shared<std::string>(std::move(*key));
    
    dataFunc(*keyPtr,
            *priceType,
            *objStr);
}

void launchWebSocketClient(net::io_context& ioc,
    ssl::context& ctx,
    net::strand<net::io_context::executor_type>& strand,
    const std::shared_ptr<BinanceRestClient>& client)
{
    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto sessHolder = std::make_shared<std::shared_ptr<session>>();

    *sessHolder = std::make_shared<session>(strand,
        ctx, 
        onPriceUpdate,
        host,
        port,
        path,
        10,
        [sessHolder, client, &strand, &ioc](const beast::error_code& ec){
            if (ec){ ioc.stop(); return;}

            auto sess = *sessHolder;
            std::thread([sess, &strand, client](){
                onGatewayConnected(sess,
                    client,
                    strand,
                    std::make_shared<Timer>(std::make_shared<Scheduler>()),    
                    std::make_shared<Worker>());
            }).detach();
        }
    );
    (*sessHolder)->run();
}

void launchRestClient(net::io_context& ioc,
    ssl::context& ctx,
    net::strand<net::io_context::executor_type>& strand)
{
    std::shared_ptr<BinanceRestClient> client =
    std::make_shared<BinanceRestClient>(strand,
        ctx,
        [&ioc, &ctx, &strand, &client]
        (const beast::error_code& ec){
            if(ec)
            {
                std::cout << "Error in Rest client init phase : " << ec.message() << std::endl;
                ioc.stop();
            }
            else
            {
                launchWebSocketClient(ioc, ctx, strand, client);
            }
        },
        10
    );
    
    client->run();
    ioc.run();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <available_brokers>" << std::endl;
        return 1;
    }

    ::availableBrokers = atoi(argv[1]);
    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto workerThread = std::make_shared<Worker>();
    auto timer = std::make_shared<Timer>(std::make_shared<Scheduler>());

    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    net::strand<net::io_context::executor_type> strand{ioc.get_executor()};

    launchRestClient(ioc, ctx, strand);

    return 0;
}

