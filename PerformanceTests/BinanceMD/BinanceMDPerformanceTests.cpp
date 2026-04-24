#include <MiddleWare/Interface.h>
#include <ConfigLib/ConfigLib.h>
#include <cstdint>
#include <openssl/evp.h>
#include <thread>
#include <type_traits>
#include "Constants.h"
#include "MDGateways/PlatformComm/SMEvents.h"
#include "RouterConfig.h"
#include "CustomTagsAndValues.h"

std::string appId;
std::string outTopic;
using Cfg = json::object;

Middleware::ProducerFunc producerFunc;
Middleware::ConsumerFunc consumerFunc;
Middleware::ShutdownFunc shutdownFunc;

uint64_t numTradeUpdatesRecd = 0;
uint64_t numDepthUpdatesRecd = 0;
uint64_t numOtherMessagesRecd = 0;

static void msgCb(const std::string& /*topic*/,
                  const int32_t& /*partition*/,
                  const int64_t& /*offset*/,
                  const std::string& msgType,
                  const std::string& key,
                  const Middleware::KeyValuePairs& /*headers*/,
                  const std::string& value)
{
    msgType == *MessageType::trade_update()?
        numTradeUpdatesRecd++:
    msgType == *MessageType::depth_update()?
        numDepthUpdatesRecd++:
    numOtherMessagesRecd++;
}

static void sencCb(const Middleware::RecordMetadata&, const Middleware::Error& err)
{
    if (!err) return;
    std::cout << "Send error: " << err.value() << " " << err.message() << "\n";
}

void sendSubUnsubs(const PriceType& priceType, bool isSub)
{
    static const std::vector<std::string> symbols{"BTCUSDT", "ETHUSDT", "XRPUSDT", "SOLUSDT", "BNBUSDT"};
    for(auto const& symbol: symbols)
    {
        ::producerFunc(
            ::outTopic,
            isSub ? MessageType::subscribe() : MessageType::unsubscribe(),
            symbol,
            json::serialize(json::object{
                {*Tags::action(),  isSub ? *TagValues::action_subscribe():
                                            *TagValues::action_unsubscribe()},
                {*Tags::destination_topic(),  ::appId},
                {*Tags::symbol(),             symbol},
                {*Tags::subscription_type(),  *priceType}
            }),
            {},
            sencCb);
    }
}

void startPerformanceTest()
{
    // sendSubUnsubs(PriceType::trade(), false);
    // sendSubUnsubs(PriceType::depth(), false);

    sendSubUnsubs(PriceType::trade(), true);
    sendSubUnsubs(PriceType::depth(), true);
    std::this_thread::sleep_for(std::chrono::seconds(30));

    sendSubUnsubs(PriceType::trade(), false);
    sendSubUnsubs(PriceType::depth(), false);
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Test completed. Trade updates received: " << numTradeUpdatesRecd
              << ", Depth updates received: " << numDepthUpdatesRecd
              << ", Other messages received: " << numOtherMessagesRecd << "\n";
}

static void initErrorCb(const Middleware::Error& err)
{
    std::cout << "IT: init error " << err.value() << " " << err.message() << "\n";
}

int main(int argc, char** argv)
{
    ::appId = argc >= 2? argv[1] : "Binance_PT_1";
    auto const& cfg_opt = Config::init(appId, [](const Cfg&){}, [](const Cfg&) {return true;});
    if(!cfg_opt) return 1;
    auto const& cfg = *cfg_opt;
    const std::string appGroup          = cfg.at(*ConfigTag::group()).as_string().c_str();
    const uint64_t heartbeatIntervalSec = cfg.at(*ConfigTag::hearbeatInterval()).as_int64();
    const uint64_t heartBeatTimeout     = cfg.at(*ConfigTag::hearbeatTimeout()).as_int64();
    ::outTopic                          = cfg.at(*CustomConfigTags::out_topic()).as_string().c_str(); 
    const std::string brokers           = cfg.at(*ConfigTag::brokers()).as_string().c_str();

    const std::string heartBeatMsg = json::serialize(json::object{{
        {*Tags::message_type(), *MessageType::heartBeat()},
        {*Tags::HeartBeatInterval(), heartbeatIntervalSec},
        {*Tags::HeartBeatTimeout(), heartBeatTimeout},
        {*Tags::appId(), appId},
        {*Tags::appGroup(), appGroup}
    }});

    const std::string desc = json::serialize(json::object({
        {*Tags::appId(), appId},
        {*Tags::appGroup(), appGroup}
    }));

    auto initCb =
    [](
        const Middleware::ProducerFunc&         pf,
        const Middleware::LowLevelProducerFunc& /*llpf*/,
        const Middleware::ConsumerFunc&         /*groupConsumerFunc*/,
        const Middleware::ConsumerFunc&         /*individualConsumerFunc*/,
        const Middleware::RequestFunc&          /*rf*/,
        const Middleware::RespondFunc&          /*respondFunc*/,
        const Middleware::ShutdownFunc&         sf)
    {
        std::cout << "Initialized middleware callbacks...\n";
        ::producerFunc  = pf;
        ::shutdownFunc = sf;

        // Launch tests on a SEPARATE thread so initCb returns immediately.
        // The middleware event loop (which dispatches responseCb / msgCb)
        // runs on the calling thread — blocking it here would starve all
        // incoming Kafka messages for the entire duration of the test.
        std::thread([](){
            startPerformanceTest();
            ::shutdownFunc();
        }).detach();
    };

    Middleware::initializeMiddleWare(appId,
        appGroup,
        [&desc](){ return desc; },
        [&heartBeatMsg] { return heartBeatMsg; },
        heartbeatIntervalSec,
        std::make_shared<ULMTTools::Timer>(std::make_shared<ULMTTools::TaskScheduler>()),
        std::make_shared<ULMTTools::WorkerThread>(), 
        msgCb,
        initCb,
        initErrorCb,
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        [](const uint64_t&, const std::string&, bool isLast){},
        [](const uint64_t&, const std::string&){},
        cfg.at(*ConfigTag::numMinBrokers()).as_int64());

    return 0;
}