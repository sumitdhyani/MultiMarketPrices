#include <boost/json/object.hpp>
#include <iostream>
#include <ranges>
#include <stdint.h>
#include <boost/json.hpp>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>
#include <UUIDGen.hpp>
#include <Logging.h>
#include <ConfigLib/ConfigLib.h>

using namespace NanoLog::LogLevels;

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";
std::string targetApp;

Middleware::ProducerFunc producerFunc;
Middleware::ConsumerFunc groupConsumerFunc;
Middleware::ConsumerFunc individualConsumerFunc;
Middleware::RequestFunc requestFunc;
Middleware::RespondFunc respondFunc;

std::string getHeartBeatMsg()
{
    return json::serialize(json::object({
        {*Tags::message_type(),*MessageType::heartBeat()},
        {*Tags::HeartBeatInterval(),5},
        {*Tags::HeartBeatTimeout(),30},
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
}

void msgCb(const std::string& topic,
    const int32_t& partition,
    const int64_t& offset,
    const std::string& msgType,
    const std::string& key,
    const Middleware::KeyValuePairs& headers,
    const std::string& value)
{
    // Process the received message (for demonstration, we just print it)
    NANO_LOG(DEBUG, "Received message from topic: %s, partition: %d, offset: %ld, msgType: %s, key: %s, value: %s",
            topic.c_str(), partition, offset, msgType.c_str(), key.c_str(), value.c_str());
    for (const auto& [headerKey, headerValue] : headers)
    {
        NANO_LOG(DEBUG, "Header - %s: %s", (*headerKey).c_str(), headerValue.c_str());
    }
}

std::string getDescMsg()
{
    return json::serialize(json::object({
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    NANO_LOG(DEBUG, "Response received: ReqId: %lu, Payload: %s, isLast: %d", reqId, msg.c_str(), isLast);
}

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    NANO_LOG(DEBUG, "Error while sending msg, code %d details: %s", err.value(), err.message().c_str());
}

void processCommand(const std::string& cmd)
{
    static UUIDGenerator gen;
    auto tokens = cmd
                | std::views::split(' ')
                | std::ranges::to<std::vector<std::string>>();

    const std::string& action = tokens[0];
    const std::string& type = tokens[1];
    if(action == "s")
    {
        const std::string& symbol = tokens[2];
        NANO_LOG(DEBUG, "Received command: %s", cmd.c_str());
        const std::string& subscriptionType = (type == "t")? *PriceType::trade() : *PriceType::depth();
        producerFunc("test_topic",
            MessageType::subscribe(),
            symbol,
            json::serialize(json::object{{*Tags::action(), *TagValues::action_subscribe()},
                {*Tags::destination_topic(), *Topic::test_topic()},
                {*Tags::symbol(), symbol},
                {*Tags::subscription_type(), subscriptionType}
            }),
            {},
            sencCb
        );
    }
    else if(action == "u")
    {
        const std::string& symbol = tokens[2];
        NANO_LOG(DEBUG, "Received command: %s", cmd.c_str());
        const std::string& subscriptionType = (type == "t")? *PriceType::trade() : *PriceType::depth();
        producerFunc("test_topic",
            MessageType::unsubscribe(),
            symbol,
            json::serialize(json::object{{*Tags::action(), *TagValues::action_unsubscribe()},
                {*Tags::destination_topic(), *Topic::test_topic()},
                {*Tags::symbol(), symbol},
                {*Tags::subscription_type(), subscriptionType}
            }),
            {},
            sencCb
        );
    }
    else if(action == "r")
    {
        NANO_LOG(DEBUG, "Received command: %s", cmd.c_str());
        if (type == "i")
        {
            requestFunc(gen.generate64(),
                    json::serialize(json::object{{*Tags::message_type(), *MessageType::instrument_request()}}),
                    targetApp,
                    sencCb
            );
            return;
        }
        const std::string& subscriptionType = (type == "t")? *PriceType::trade() : *PriceType::depth();
        const std::string& symbol = tokens[2];
        requestFunc(gen.generate64(),
                    json::serialize(json::object{{*Tags::symbol(), symbol},
                        {*Tags::subscription_type(), subscriptionType},
                        {*Tags::message_type(), *MessageType::price_request()}
                    }),
                    targetApp,
                    sencCb
        );
    }
}

void mainLoop()
{
    while(true)
    {
        std::cout << "Enter next command" << std::endl;
        std::string cmd;
        std::getline(std::cin, cmd);
        processCommand(cmd);
    }
}

void initErrorCb(const Middleware::Error& err)
{
    NANO_LOG(DEBUG, "Error while initializing Middleware: %d, %s", err.value(), err.message().c_str());
}

void runningErrorCb(const Middleware::Error& error) {
    NANO_LOG(DEBUG, "[ERROR CALLBACK] Code: %d | Message: %s | Fatal: %s",
            error.value(), error.message().c_str(),
            error.isFatal() ? "YES" : "NO");

    if (error.value() == RD_KAFKA_RESP_ERR__TRANSPORT ||
        error.value() == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN ||
        error.value() == RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION) {
        
        NANO_LOG(DEBUG, ">>> Broker Disconnected / Network issue detected! Reconnecting...");
    }
    else
    {
        NANO_LOG(DEBUG, "Error from middleware, details: %s", error.message().c_str());
    }

    if (error.isFatal()) {
        NANO_LOG(DEBUG, ">>> FATAL ERROR! Application should probably shutdown/restart.");
    }
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

int main(int argc, char* argv[])
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

    auto scheduler = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer =  std::make_shared<ULMTTools::Timer>(scheduler);
    
    auto initCb =
    [timer](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc,
        const Middleware::RespondFunc& respondFunc)
    {
        ::producerFunc = producerFunc;
        ::groupConsumerFunc = groupConsumerFunc;
        ::individualConsumerFunc = individualConsumerFunc;
        ::requestFunc = requestFunc;
        ::respondFunc = respondFunc;

        std::thread(mainLoop).detach();
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    const std::string brokers = cfg.at(*ConfigTag::brokers()).as_string().c_str();
    ::targetApp = cfg.at("targetApp").as_string().c_str();

    NANO_LOG(DEBUG, "Initializing middleware");
    Middleware::initializeMiddleWare(appId,
        *AppGroup::DataDumper(),
        [appStr]() { return appStr; },
        [heartBeatStr]() { return heartBeatStr; },
        5,
        timer,
        std::make_shared<ULMTTools::WorkerThread>(),
        msgCb,
        initCb,
        initErrorCb,
        {
            {MiddlewareConfig::bootstrap_servers(), brokers},
        },
        {
            {MiddlewareConfig::bootstrap_servers(), brokers}
        },
        responseCb,
        [](const uint64_t&, const std::string&){ NANO_LOG(DEBUG, "Received request without a handler"); },
        cfg.at(*ConfigTag::numMinBrokers()).as_int64()
    );
}
