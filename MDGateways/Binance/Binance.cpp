#include <PlatformComm/PlatformComm.h>
#include "WebSockets.h"

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

DataFunc dataFunc;

void transformForPlatform(json::object& update)
{
    if (update.contains("s")) update["symbol"] = update["s"].as_string();
    if (update.contains("p")) update["price"] = update["p"].as_string();
    if (update.contains("q")) update["quantity"] = update["q"].as_string();

    update.erase("s");
    update.erase("p");
    update.erase("q");
}

int main(int argc, char** argv)
{
    char const* host = "stream.binance.com";
    char const* port = "9443";
    auto const path = "/stream";

    auto workerThread = std::make_shared<ULMTTools::WorkerThread>();
    auto scheduler = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer = std::make_shared<ULMTTools::Timer>(scheduler);

    // The io_context is required for all I/O
    net::io_context ioc;

    // The SSL context is required, and holds certificates
    ssl::context ctx{ssl::context::tlsv12_client};

    // Launch the asynchronous operation
    // std::shared_ptr<session> sess;
    std::shared_ptr<session> sess = std::make_shared<session>(ioc,
        ctx, 
        [workerThread](const std::string& update) {
            auto obj = std::make_shared<json::object>(json::parse(update).as_object());
            transformForPlatform(*obj);
            auto objStr = std::make_shared<std::string>(json::serialize(*obj));
            workerThread->push([obj, objStr](){
                dataFunc(obj->at("symbol").as_string().c_str(),
                        obj->contains("bids") ? PriceType::depth() : PriceType::trade(),
                        *objStr);
            });
        },
        host,
        port,
        path,
        10,
        [](const beast::error_code& ec, bool isFatal){},
        [&sess, timer, workerThread](){
            initMiddleware("node_2:9092,node_3:9092",
                [&sess](const std::string& symbol, const PriceType& priceType){
                    priceType == PriceType::trade() ?
                        sess->subscribeTrade(symbol) :
                        sess->subscribeDepth(symbol);
                },
                [&sess](const std::string& symbol, const PriceType& priceType){
                    priceType == PriceType::trade() ?
                        sess->unsubscribeTrade(symbol) :
                        sess->unsubscribeDepth(symbol);
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
        });

    sess->run();
    ioc.run();

    return 0;
}

