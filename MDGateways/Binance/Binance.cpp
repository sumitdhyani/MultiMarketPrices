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
#include <UUIDGen.hpp>

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";
std::string price_subscriptions_topic = "binance_price_subscriptions";

std::unordered_map<int32_t, std::unique_ptr<PerPartitionFSM>> partitionFSMs;

void rebalanceCallback(const TopicAssignmentEvent& rebalanceType,
                        const std::string& topic,
                        const int32_t& partition,
                        const Middleware::ProducerFunc& producerFunc)
{
    if (rebalanceType == TopicAssignmentEvent::PartitionAssigned)
    {
        if (auto it = partitionFSMs.find(partition); it == partitionFSMs.end())
        {
            auto newFsm =
            std::make_unique<PerPartitionFSM>(partition,
                [](const std::string&, const std::string&){},
                [](const std::string&, const std::string&){},
                [producerFunc](const Instrument& instrument,
                    const SubscriptionType& subType,
                    const bool& isSub)
                {
                    // Logic o send sync data
                }
            );
            
            newFsm->start();
            partitionFSMs[partition] = std::move(newFsm);
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
    if(!err) return;
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

    std::string group = jsonObj.at(*Tags::group_identifier()).as_string().c_str();
    auto const tokens = group
                | std::views::split(',')
                | std::ranges::to<std::vector<std::string>>();
    
    if (tokens.size() != 3)
    {
        // Log error
        return;
    }

    int32_t partition = atoi((*tokens.rbegin()).c_str());

    if (auto it = partitionFSMs.find(partition); 
        it != partitionFSMs.end())
    {
        auto& [_, existingFSM] = *it;
        json::object obj = json::parse(msg).as_object();

        isLast?
            existingFSM->handleEvent(SubscriptionRecord(jsonObj)) :
            existingFSM->handleEvent(DownloadEnd(jsonObj));
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
    [](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc)
    {
        UUIDGenerator gen;
        std::string serviceGroup = "binance_price_fetcher";
        std::string topic = "binance_price_subscriptions";

        requestFunc(gen.generate64(),
                    json::serialize(json::object{{
                        {"group", serviceGroup + ":" + topic + ":0"},
                        {"destination_topic", "test_topic"}
                    }}),
                    "pubSub_sync_data_requests",
                    sencCb
        );

        groupConsumerFunc(price_subscriptions_topic,
            [producerFunc](const TopicAssignmentEvent& rebalanceType,
                const std::string& topic,
                const int32_t& partition) {
                rebalanceCallback(rebalanceType,
                                    topic,
                                    partition,
                                    producerFunc);
            }
        );
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
