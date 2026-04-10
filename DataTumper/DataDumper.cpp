#include <iostream>
#include <chrono>
#include <ranges>
#include <system_error>
#include <stdint.h>
#include <boost/json.hpp>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>
#include <UUIDGen.hpp>

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";

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
    std::cout << "Received message from topic: " << topic
            << ", partition: " << partition
            << ", offset: " << offset
            << ", msgType: " << msgType
            << ", key: " << key
            << ", value: " << value
            << ", headers: {";
    for (const auto& [headerKey, headerValue] : headers)
    {
        std::cout << *headerKey << ": " << headerValue << ", ";
    }

    std::cout << "}" << std::endl;
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
    std::cout   << "Response received:" << std::endl
                << "ReqId: " << reqId << std::endl
                << "Payload: " << msg << std::endl
                << "isLast: " << isLast << std::endl;
}

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    std::cout << "Error while sending msg, code "
                << err.value() << " details: "
                << err.message() << std::endl;
}

void processCommand(const std::string& cmd)
{
    static UUIDGenerator gen;   
    auto tokens = cmd
                | std::views::split(' ')
                | std::ranges::to<std::vector<std::string>>();

    const std::string& action = tokens[0];
    const std::string& type = tokens[1];
    const std::string& symbol = tokens[2];
    if(action == "s")
    {
        std::cout << "Received command: " << cmd << std::endl;
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
        std::cout << "Received command: " << cmd << std::endl;
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
        std::cout << "Received command: " << cmd << std::endl;
        const std::string& subscriptionType = (type == "t")? *PriceType::trade() : *PriceType::depth();
        requestFunc(gen.generate64(),
                    json::serialize(json::object{{*Tags::symbol(), symbol},
                        {*Tags::subscription_type(), subscriptionType}
                    }),
                    "binance_price_fetcher_node_6",
                    sencCb
        );
    }
}

void mainLoop()
{
    while(true)
    {
        std::cout << "ENter next command" << std::endl;
        std::string cmd;
        std::getline(std::cin, cmd);
        processCommand(cmd);
    }
}

void initErrorCb(const Middleware::Error& err)
{
    std::cout << "Error while initializing Middleware: " << err.value() << ", " << err.message() << std::endl;
}

void runningErrorCb(const Middleware::Error& error) {
    std::cout << "[ERROR CALLBACK] Code: " << error.value() 
                << " | Message: " << error.message() 
                << " | Fatal: " << (error.isFatal() ? "YES" : "NO") 
                << std::endl;

    // Connection related errors handle karo
    if (error.value() == RD_KAFKA_RESP_ERR__TRANSPORT ||
        error.value() == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN ||
        error.value() == RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION) {
        
        std::cout << ">>> Broker Disconnected / Network issue detected! Reconnecting...\n";
    }
    else
    {
        std::cout << "Error from middleware, details: " << error.message() << std::endl;
    }

    if (error.isFatal()) {
        std::cerr << ">>> FATAL ERROR! Application should probably shutdown/restart.\n";
    }
}

int main()
{
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

    std::string brokers = "127.0.0.1:9092";
    std::cout << "Initializing middleware" << std::endl;
    Middleware::initializeMiddleWare("DataDumper",
        "DataDumperGrp",
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
            {MiddlewareConfig::bootstrap_servers(), brokers},
            {MiddlewareConfig::group_id(), "DataDumperApp"}
        },
        responseCb,
        [](const uint64_t&, const std::string&){ std::cout << "Received request without a handler" << std::endl; },
        1
    );
}
