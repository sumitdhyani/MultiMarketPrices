#include <iostream>
#include <chrono>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <CommonUtils/PropertyTree.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>

using PropertyTree = ULCommonUtils::PropertyTree<std::string, std::string, int>;
using Timer = ULMTTools::Timer;

#define deserializer ULCommonUtils::deseraliseFromJSon
#define serializer ULCommonUtils::serializeToJSon<ULCommonUtils::NullVisitor, std::string, std::string, int>
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";

std::string getHeartBeatMsg()
{
    return serializer(PropertyTree({
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
    return serializer(PropertyTree({
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
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
        // Yahan tum reconnection logic daal sakte ho
        // ya alert bhej sakte ho (Prometheus, Slack, email etc.)
    }

    if (error.isFatal()) {
        std::cerr << ">>> FATAL ERROR! Application should probably shutdown/restart.\n";
    }
}

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    std::cout << "Error while sending msg, code "
                << err.value() << " details: "
                << err.message() << std::endl;
}

int main()
{
    auto scheduler = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer =  std::make_shared<ULMTTools::Timer>(scheduler);
    
    auto initCb =
    [timer](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc)
    {
        std::cout << "Inside init callback" << std::endl;
        timer->install(
            [producerFunc]() {
            producerFunc(*Topic::test_topic(),
                        MessageType::heartBeat(),
                        "dummy_app",
                        getHeartBeatMsg(),
                        {},
                        sencCb);
            },
            std::chrono::seconds(5)
        );

        individualConsumerFunc(*Topic::prices(), nullptr);
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    std::string brokers = "node_2:9092,node_3:9092";
    std::cout << "Initializing middleware" << std::endl;
    Middleware::initializeMiddleWare("DataDumperApp",
        "DataDumperGroup",
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
        }
    );
}
