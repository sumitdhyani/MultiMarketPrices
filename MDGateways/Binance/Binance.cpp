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
    
    if(!obj.contains("data"))
    {
        std::cout << "data tag missing" << std::endl;
        return;
    }
    
    obj = std::move(obj["data"].as_object());
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

    // Launch the asynchronous operation
    std::shared_ptr<session> sess = std::make_shared<session>(strand,
        ctx, 
        onPriceUpdate,
        host,
        port,
        path,
        10,
        [](const beast::error_code& ec, bool isFatal){},
        [&sess, &strand](){
            std::thread([&sess, &strand](){
                onGatewayConnected(sess,
                    strand,
                    std::make_shared<Timer>(std::make_shared<Scheduler>()),    
                    std::make_shared<Worker>());
            }).detach();
        }
    );


    sess->run();
    ioc.run();

    return 0;
}

