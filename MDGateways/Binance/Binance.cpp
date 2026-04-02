#include <PlatformComm/PlatformComm.h>
#include "WebSockets.h"

DataFunc dataFunc;
bool middlewareInitialized = false;

using Worker = ULMTTools::WorkerThread;
using Worker_SPtr = std::shared_ptr<Worker>;

using Scheduler = ULMTTools::TaskScheduler;
using Scheduler_SPtr = std::shared_ptr<Scheduler>;

using Timer = ULMTTools::Timer;
using Timer_SPtr = std::shared_ptr<Timer>;



void initMiddleware(const std::string& brokers,
        const PubSubFunc& subFunc,
        const PubSubFunc& unsububFunc,
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
        registrationFunc,
        timer,
        workerThread,
        appId,
        appGroup,
        inTopic,
        initErrorCb);
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


void subscribe(const std::shared_ptr<session>& sess,
    const std::string& symbol,
    const PriceType& priceType)
{
    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(), lowerSymbol.end(), lowerSymbol.begin(), ::tolower);
    priceType == PriceType::trade() ?
        sess->subscribeTrade(lowerSymbol) :
        sess->subscribeDepth(lowerSymbol);
}

void unsubscribe(const std::shared_ptr<session>& sess,
    const std::string& symbol,
    const PriceType& priceType)
{
    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(), lowerSymbol.end(), lowerSymbol.begin(), ::tolower);
    priceType == PriceType::trade() ?
        sess->unsubscribeTrade(lowerSymbol) :
        sess->unsubscribeDepth(lowerSymbol);
}

void onGatewayConnected(const std::shared_ptr<session>& sess,
    const Timer_SPtr& timer,
    const Worker_SPtr& workerThread)
{
    if (middlewareInitialized) return;
    middlewareInitialized = true;
    
    initMiddleware("node_2:9092,node_3:9092",
        [&sess](const std::string& symbol, const PriceType& priceType){
            std::cout << "Subscribing to " << symbol << " with price type " << *priceType << std::endl;
            subscribe(sess, symbol, priceType);
        },
        [&sess](const std::string& symbol, const PriceType& priceType){
            std::cout << "Unsubscribing from " << symbol << " with price type " << *priceType << std::endl;
            unsubscribe(sess, symbol, priceType);
        },
        [](const DataFunc& dataFunc){
            // Registering the callback to receive price data from the exchange
            ::dataFunc = dataFunc;
        },
        timer,
        workerThread,
        "binance_price_fetcher_node_4",
        "binance_price_fetcher_4",
        "test_topic",
        [](const Middleware::Error& error){
            std::cerr << "Error initializing middleware: " << error.message() << "\n";
        });
}

void onPriceUpdate(const std::string& update,
    const Worker_SPtr& appWorker)
{
    auto obj = std::make_shared<json::object>(std::move(json::parse(update).as_object()));
    
    if(!obj->contains("data"))
    {
        std::cout << "data tag missing" << std::endl;
        return;
    }
    
    obj = std::make_shared<json::object>(std::move((*obj)["data"].as_object()));
    
    if (!transformForPlatform(*obj)) 
    {
        std::cout << "Object transformation failed, update was: " << update << std::endl;
        return;
    }

    auto objStr = std::make_shared<std::string>(std::move(json::serialize(*obj)));
    appWorker->push([obj, objStr](){
        dataFunc(obj->at("symbol").as_string().c_str(),
                obj->contains("bids") ? PriceType::depth() : PriceType::trade(),
                *objStr);
    });
}
int main(int argc, char** argv)
{
    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto workerThread = std::make_shared<Worker>();
    auto timer = std::make_shared<Timer>(std::make_shared<Scheduler>());

    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};

    // Launch the asynchronous operation
    std::shared_ptr<session> sess = std::make_shared<session>(ioc,
        ctx, 
        [workerThread](const std::string& update) {
            onPriceUpdate(update, workerThread);
        },
        host,
        port,
        path,
        10,
        [](const beast::error_code& ec, bool isFatal){},
        [&sess, timer, workerThread](){
            std::thread([&sess, timer, workerThread](){
                onGatewayConnected(sess, timer, workerThread);
            }).detach();
        }
    );

    sess->run();
    ioc.run();

    return 0;
}

