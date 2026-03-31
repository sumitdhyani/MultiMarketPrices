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
#include <PerPartitionSM.h>

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";

std::unordered_map<int32_t, std::unique_ptr<PerPartitionFSM>> partitionFSMs;

void rebalanceCallback(const TopicAssignmentEvent& rebalanceType,
                        const std::string& topic,
                        const int32_t& partition)
{
    if (rebalanceType == TopicAssignmentEvent::PartitionAssigned)
    {
        if (auto it = partitionFSMs.find(partition); it == partitionFSMs.end())
        {
            partitionFSMs[partition] = 
            std::make_unique<PerPartitionFSM>(partition,
                nullptr, nullptr, nullptr);
            partitionFSMs[partition]->start();
        }
        else
        {
            auto const& [_, existingFSM] = *it;
            existingFSM->handleEvent(Assign{});
        }
    }
    else if (rebalanceType == TopicAssignmentEvent::PartitionRevoked)
    {
        if (auto it = partitionFSMs.find(partition); it != partitionFSMs.end())
        {
            auto const& [_, existingFSM] = *it;
            existingFSM->handleEvent(Revoke{});
        }
        else
        {
            // Log error should never be here
        }
    }
}

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


bool findAndExec(const uint32_t& partition,
                 const std::function<void(PerPartitionFSM&)>& func)
{
    bool ret = true;
    if (auto it = partitionFSMs.find(partition); it != partitionFSMs.end())
    {
        auto const& [_, existingFSM] = *it;
        func(*existingFSM);
    }
    else ret = false;

    return ret;
}

void msgCb(const std::string& topic,
    const int32_t& partition,
    const int64_t& offset,
    const std::string& msgType,
    const std::string& key,
    const Middleware::KeyValuePairs& headers,
    const std::string& msg)
{
    if (msgType == *MessageType::subscribe() ||
        msgType == *MessageType::unsubscribe())
    {
        auto it = partitionFSMs.end();
        it = partitionFSMs.find(partition);
        if (auto it = partitionFSMs.find(partition); 
            it != partitionFSMs.end())
        {
            auto& [_, existingFSM] = *it;
            json::object obj = json::parse(msg).as_object();
            existingFSM->handleEvent(Instrument(obj.at(*Tags::symbol()).as_string().c_str()),
                            SubscriptionType(obj.at(*Tags::subscription_type()).as_string().c_str()),
                            msgType == *MessageType::subscribe());
        }
    }
}

std::string getDescMsg()
{
    return json::serialize(json::object({
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
}


void initErrorCb(const Middleware::Error& err)
{
    std::cout << "Error while initializing Middleware: " << err.value() << ", " << err.message() << std::endl;
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    
    std::error_code ec;
    json::object jsonObj = json::parse(msg, ec).as_object();
    if (ec) {
        std::cerr << "Error parsing JSON: " << ec.message() << std::endl;
    } else {
        std::cout << "Parsed JSON content: " << json::serialize(jsonObj) << std::endl;
    }

    if(!isLast)
    {
        std::string params = jsonObj["key"].as_string().c_str();
        std::erase_if(params, [](const char c) {
            return c == '\'' || c == '(' || c == '#' || c == ')' || c == ' ';
        });

        auto tokens = params
                | std::views::split(',')
                | std::ranges::to<std::vector<std::string>>();
        
        
        std::cout << "Cleaned params: " << std::endl;
        for(auto const& token: tokens)
        {
            std::cout << token << std::endl;
        }
    }
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
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc)
    {
        // producerFunc("sdp_node_3",
        //             MessageType::heartBeat(),
        //             "dummy_app",
        //             getHeartBeatMsg(),
        //             {},
        //             sencCb);
        // std::cout << "Inside init callback" << std::endl;
        // timer->install(
        //     [producerFunc]() {
        //     producerFunc(*Topic::test_topic(),
        //                 MessageType::heartBeat(),
        //                 "dummy_app",
        //                 getHeartBeatMsg(),
        //                 {},
        //                 sencCb);
        //     },
        //     std::chrono::seconds(5)
        // );

        std::string serviceGroup = "binance_price_fetcher";
        std::string topic = "binance_price_subscriptions";
        individualConsumerFunc(*Topic::test_topic(), nullptr);
        for(uint16_t partition = 0, reqId = 1; partition < 11; ++partition, ++reqId)
        {
            std::string group = serviceGroup + ":" + topic + ":" + std::to_string(partition);
            requestFunc(++reqId,
                        json::serialize(json::object{{
                            {"group", group},
                            {"destination_topic", "test_topic"}
                        }}),
                        "pubSub_sync_data_requests",
                        sencCb
            );
        }

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
        },
        responseCb
    );
}
