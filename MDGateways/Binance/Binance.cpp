#include <PlatformComm/PlatformComm.h>
#include "WebSockets.h"

DataFunc dataFunc;
bool middlewareInitialized = false;

void initMiddleware(const std::string& brokers,
        const PubSubFunc& subFunc,
        const PubSubFunc& unsububFunc,
        const std::function<void(const DataFunc&)>& registrationFunc,
        const std::shared_ptr<ULMTTools::Timer>& timer,
        const std::shared_ptr<ULMTTools::WorkerThread>& workerThread,
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


void transformForPlatform(json::object& update)
{
    if(!update.contains("s") || !update.contains("p") || !update.contains("q"))
    {
        // Log some error here about unexpected message format
        return;
    }

    update["symbol"] = update["s"].as_string();
    update["price"] = update["p"].as_string();
    update["quantity"] = update["q"].as_string();
    
    update.erase("s");
    update.erase("p");
    update.erase("q");
}


void subscribe(const std::shared_ptr<session>& sess,
    const std::string& symbol,
    const PriceType& priceType)
{
    priceType == PriceType::trade() ?
        sess->subscribeTrade(symbol) :
        sess->subscribeDepth(symbol);
}

void unsubscribe(const std::shared_ptr<session>& sess,
    const std::string& symbol,
    const PriceType& priceType)
{
    priceType == PriceType::trade() ?
        sess->unsubscribeTrade(symbol) :
        sess->unsubscribeDepth(symbol);
}

void onGatewayConnected(const std::shared_ptr<session>& sess,
    const std::shared_ptr<ULMTTools::Timer>& timer,
    const std::shared_ptr<ULMTTools::WorkerThread>& workerThread)
{
    if (middlewareInitialized) return;
    middlewareInitialized = true;
    
    initMiddleware("node_2:9092,node_3:9092",
        [&sess](const std::string& symbol, const PriceType& priceType){
            subscribe(sess, symbol, priceType);
        },
        [&sess](const std::string& symbol, const PriceType& priceType){
            unsubscribe(sess, symbol, priceType);
        },
        [](const DataFunc& dataFunc){
            // Registering the callback to receive price data from the exchange
            ::dataFunc = dataFunc;
        },
        timer,
        workerThread,
        "binance_price_fetcher_node_4",
        "binance_price_fetcher",
        "binance_price_subscriptions",
        [](const Middleware::Error& error){
            std::cerr << "Error initializing middleware: " << error.message() << "\n";
        });
}

void onPriceUpdate(const std::string& update,
    const std::shared_ptr<ULMTTools::WorkerThread>& appWorker)
{
    auto obj = std::make_shared<json::object>(json::parse(update).as_object());
    transformForPlatform(*obj);
    auto objStr = std::make_shared<std::string>(json::serialize(*obj));
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

    auto workerThread = std::make_shared<ULMTTools::WorkerThread>();
    auto timer = std::make_shared<ULMTTools::Timer>(std::make_shared<ULMTTools::TaskScheduler>());

    // The io_context is required for all I/O
    net::io_context ioc;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ssl::context::tlsv12_client};

    // Launch the asynchronous operation
    // std::shared_ptr<session> sess;
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
            onGatewayConnected(sess, timer, workerThread);
        }
    );

    sess->run();
    ioc.run();

    return 0;
}

