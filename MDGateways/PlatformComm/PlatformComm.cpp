#include <NanoLog.h>
#include <NanoLogCpp17.h>
#include <boost/json/array.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <exception>
#include <string>
#include<unordered_set>
#include<unordered_map>
#include "PlatformComm.h"
#include "Constants.h"
#include "PerPartitionSM.h"
#include <Logging.h>

using namespace NanoLog::LogLevels;

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId;
std::string appGroup;
std::string inTopic;
SubUnsubFunc subFunc;
SubUnsubFunc unsubFunc;
KeyGenFunc keyGenFunc;
GetPriceSnapshotFunc getPriceSnapshotFunc;
GetInstrumentListFunc getInstrumentListFunc;
std::unordered_map<std::string, std::unordered_set<std::string>> symbolToDestTopics;
std::unordered_map<std::string, std::unordered_set<std::string>> destTopicToSymbols;

std::unordered_map<int32_t, std::unique_ptr<PerPartitionFSM>> partitionFSMs;
Middleware::ProducerFunc producerFunc;
Middleware::RequestFunc requestFunc;
Middleware::RespondFunc respondFunc;

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err)
{
    if(!err) return;
    NANO_LOG(DEBUG, "Error while sending msg, code %d details: %s", err.value(), err.message().c_str());
}

void rebalanceCallback(const TopicAssignmentEvent& rebalanceType,
                        const std::string& topic,
                        const int32_t& partition)
{
    if (rebalanceType == TopicAssignmentEvent::PartitionAssigned)
    {
        if (auto it = partitionFSMs.find(partition); it == partitionFSMs.end())
        {
            auto newFsm =
            std::make_unique<PerPartitionFSM>(partition,
                subFunc,
                unsubFunc
            );

            static UUIDGenerator gen;
            const std::string group = appGroup + ":" + inTopic + ":" + std::to_string(partition);

            uint64_t reqId = gen.generate64();
            NANO_LOG(DEBUG, "Requesting for group info for group: %s with reqId: %lu", group.c_str(), reqId);
            requestFunc(reqId,
                        json::serialize(json::object{{
                            {*Tags::group_identifier(), group},
                            {*Tags::destination_topic(), appId}
                        }}),
                        *Topic::pubSub_sync_data_requests(),
                        sencCb
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


bool handleBookKeepingForSubscription(const std::string& key, const std::string& destTopic)
{   
    if (auto it = symbolToDestTopics.find(key); it == symbolToDestTopics.end())
    {
        symbolToDestTopics[key] = {destTopic};
        return true;
    }
    else
    {
        auto& [_, destTopics] = *it;
        return destTopics.insert(destTopic).second;
    }
}

bool handleBookKeepingForUnsubscription(const std::string& key, const std::string& destTopic)
{   
    auto it = symbolToDestTopics.find(key);
    if (it == symbolToDestTopics.end()) return false;
 
    auto& [_, destTopics] = *it;
    if(destTopics.erase(destTopic))
    {
        if(destTopics.empty()) symbolToDestTopics.erase(it);
        return true;
    }
    return false;
}

bool handleBookKeeping(const std::string& key, const std::string& destTopic, bool isSub)
{
    // This function will handle all the bookkeeping related tasks like maintaining the symbol to destination topic mapping and vice versa
    // This will be called whenever there is a subscribe or unsubscribe event
    // The mapping will be used to route the price updates to the correct destination topics
    return isSub ? handleBookKeepingForSubscription(key, destTopic) :
                   handleBookKeepingForUnsubscription(key, destTopic);
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
        auto it = partitionFSMs.find(partition);
        if (it == partitionFSMs.end()) return;

        auto obj = json::parse(msg).as_object();
        auto key = keyGenFunc(obj);
        if (!key)   return;

        bool isSub = msgType == *MessageType::subscribe();
        const std::string destTopic = obj.at(*Tags::destination_topic()).as_string().c_str();

        if (!handleBookKeeping(*key, destTopic, isSub))
        {
            NANO_LOG(DEBUG, "BookKeeping failed for key: %s and destTopic: %s", (*key).c_str(), destTopic.c_str());
            return;
        }
        
        auto& [_, existingFSM] = *it;
        existingFSM->handleEvent(SubUnsubKey(*key), isSub);
        
        std::string group = appGroup + ":" + inTopic + ":" + std::to_string(partition);
        producerFunc(*Topic::pubSub_sync_data(),
            MessageType::sync_data_update(),
            appId,
            json::serialize(json::object{
                {*Tags::group_identifier(), group},
                {*Tags::subscriptionKey(), *key},
                {*Tags::action(), isSub ?
                    *TagValues::action_subscribe() :
                    *TagValues::action_unsubscribe()},
                {*Tags::destination_topic(), destTopic}
            }),
            {},
            sencCb
        );
    }
    else
    {
        NANO_LOG(ERROR, "Unknown msgType received: %s", msgType.c_str());
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
    NANO_LOG(DEBUG, "Error while initializing Middleware: %d, %s", err.value(), err.message().c_str());
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    NANO_LOG(DEBUG, "Received response: %s", msg.c_str());
    std::error_code ec;
    json::object jsonObj = json::parse(msg, ec).as_object();
    if (ec) {
        NANO_LOG(DEBUG, "Error parsing JSON: %s", ec.message().c_str());
        return;
    } else {
        NANO_LOG(DEBUG, "Parsed JSON content: %s", json::serialize(jsonObj).c_str());
    }

    NANO_LOG(DEBUG, "Parsing group from json message");
    std::string group = jsonObj.at(*Tags::group_identifier()).as_string().c_str();
    auto const tokens = group
                | std::views::split(':')
                | std::ranges::to<std::vector<std::string>>();
    
    if (tokens.size() != 3)
    {
        // Log error
        NANO_LOG(DEBUG, "Error: Invalid group identifier format");
        return;
    }

    int32_t partition = atoi((*tokens.rbegin()).c_str());
    NANO_LOG(DEBUG, "Parsed partition from group identifier: %s", (*tokens.rbegin()).c_str());

    if (auto it = partitionFSMs.find(partition); 
        it != partitionFSMs.end())
    {
        auto& [_, existingFSM] = *it;
        !isLast?
            existingFSM->handleEvent(SubscriptionRecord(jsonObj)) :
            existingFSM->handleEvent(DownloadEnd(jsonObj));
    }
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

void onPriceDataFromExchange(const std::string& key,
                            const PriceType& priceType,
                            const std::string& update)
{
    if (auto it = symbolToDestTopics.find(key); it != symbolToDestTopics.end())
    {
        for (auto const& destTopic : it->second)
        {
            producerFunc(destTopic,
                priceType == *priceType ?
                    MessageType::depth_update() :
                    MessageType::trade_update(),
                key,
                update,
                {}, 
                sencCb);
        }
    }
    else
    {
        NANO_LOG(DEBUG, "No destination topic found for key: %s", key.c_str());
    }
}

void handlePriceRequest(const uint64_t& reqId, const json::object& obj)
{
    auto priceType = strToPriceType(obj.at(*Tags::subscription_type()).as_string().c_str());
    if(!priceType) return;
            
    getPriceSnapshotFunc(obj.at(*Tags::symbol()).as_string().c_str(),
                        *priceType,
                        [reqId]
                        (const boost::json::object& snapshot, const std::string& err){
                            if(!err.empty()) return;
                            respondFunc(reqId, json::serialize(snapshot), true, sencCb);
                        }
                    );
}

void handleInstrumentListRequest(const uint64_t& reqId, const json::object& obj)
{
    NANO_LOG(NOTICE, "Received instrument request");
    getInstrumentListFunc([reqId](const SymbolStore& symbolStore){
        NANO_LOG(NOTICE, "Received instrument request, total %d instruments provided by market interface", (int)symbolStore.size());
        for(auto const& [_, symbolDetails] : symbolStore)
        {
            respondFunc(reqId, symbolDetails, false, sencCb);
        }
        respondFunc(reqId, "", true, sencCb);
    });
}

void PlatformComm::init(const std::string& brokers,
            const SubUnsubFunc& subFunc,
            const SubUnsubFunc& unsubFunc,
            const GetPriceSnapshotFunc& getPriceSnapshotFunc,
            const GetInstrumentListFunc& getInstrumentListFunc,
            const KeyGenFunc& keyGenFunc,
            const std::function<void(const DataFunc&)>& registrationFunc,
            const std::shared_ptr<ULMTTools::Timer> timer,
            const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
            const std::string& appId,
            const std::string& appGroup,
            const std::string& inTopic,
            const Middleware::ErrCallback& initErrorCb,
            const uint16_t& minAvailableBrokers)
{
    ::appId = appId;
    ::appGroup = appGroup;
    ::inTopic = inTopic;
    ::subFunc = subFunc;
    ::unsubFunc = unsubFunc;
    ::keyGenFunc = keyGenFunc;
    ::getPriceSnapshotFunc = getPriceSnapshotFunc;
    ::getInstrumentListFunc = getInstrumentListFunc;
    
    registrationFunc(
        [workerThread](const std::string& key, const PriceType& priceType, const std::string& update )
        {
            workerThread->push([key, priceType, update](){
                onPriceDataFromExchange(key, priceType, update);
            });
        }
    );
    
    auto initCb =
    [](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc,
        const Middleware::RespondFunc& respondFunc)
    {
        ::producerFunc = producerFunc;
        ::requestFunc = requestFunc;
        ::respondFunc = respondFunc;
        
        if (::inTopic != ::appId)
        {
            groupConsumerFunc(::inTopic, rebalanceCallback);
        }

    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    NANO_LOG(DEBUG, "Initializing middleware");
    Middleware::initializeMiddleWare(appId,
        appGroup,
        [appStr]() { return appStr; },
        [heartBeatStr]() { return heartBeatStr; },
        5,
        timer,
        workerThread,
        msgCb,
        initCb,
        initErrorCb,
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        responseCb,
        [](const uint64_t& reqId, const std::string& payLoad){
            const json::object obj = json::parse(payLoad).as_object();
            const std::string msgType = obj.at(*Tags::message_type()).as_string().c_str();
            if (msgType == *MessageType::price_request())
            {
                handlePriceRequest(reqId, obj);
            }
            else if (msgType == *MessageType::instrument_request())
            {
                handleInstrumentListRequest(reqId, obj);
            }
        },
        minAvailableBrokers
    );
}
