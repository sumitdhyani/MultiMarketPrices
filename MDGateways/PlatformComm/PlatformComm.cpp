#include<unordered_set>
#include<unordered_map>
#include "PlatformComm.h"
#include "PerPartitionSM.h"

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId;
std::string appGroup;
std::string inTopic;
SubUnsubFunc subFunc;
SubUnsubFunc unsubFunc;
KeyGenFunc keyGenFunc;
std::unordered_map<std::string, std::unordered_set<std::string>> symbolToDestTopics;
std::unordered_map<std::string, std::unordered_set<std::string>> destTopicToSymbols;

std::unordered_map<int32_t, std::unique_ptr<PerPartitionFSM>> partitionFSMs;
Middleware::ProducerFunc producerFunc;
Middleware::RequestFunc requestFunc;

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err)
{
    if(!err) return;
    std::cout << "Error while sending msg, code "
                << err.value() << " details: "
                << err.message() << std::endl;
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
            std::cout << "Requesting for group info for group: " << group << " with reqId: " << reqId << std::endl;
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

InstrumentType strToInstrumentType(const std::string& instrumentType)
{
    if (instrumentType == *InstrumentType::spot())
    {
        return InstrumentType::spot();
    }
    else if (instrumentType == *InstrumentType::future())
    {
        return InstrumentType::future();
    }
    else if (instrumentType == *InstrumentType::option())
    {
        return InstrumentType::option();
    }

    throw std::invalid_argument("Invalid instrument type: " + instrumentType);
}

OptionType strToOptionType(const std::string& instrumentType)
{
    if (instrumentType == *OptionType::call())
    {
        return OptionType::call();
    }
    else if (instrumentType == *OptionType::put())
    {
        return OptionType::put();
    }

    throw std::invalid_argument("Invalid option type: " + instrumentType);
}

std::optional<std::string> generateKey(const json::object& obj)
{
    try
    {
        Instrument instrument(obj.at(*Tags::symbol()).as_string().c_str());
        InstrumentType instrumentType = 
        obj.contains(*InstrumentAttributes::instrument_type()) ?
            strToInstrumentType(obj.at(*InstrumentAttributes::instrument_type()).as_string().c_str()) :
            InstrumentType::spot();
            
        std::string key = *instrument + ":" +
                            obj.at(*Tags::subscription_type()).as_string().c_str() + ":" +
                            *instrumentType;
        if(instrumentType == InstrumentType::option())
        {
            key += ":" + *strToOptionType(obj.at(*InstrumentAttributes::option_type()).as_string().c_str());
        }
        return key;
    }
    catch(const std::exception& e)
    {
        std::cout << "Error generating key: " << e.what() << std::endl;
        return std::nullopt;
    }

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

        if (!handleBookKeeping(*(key.value()), destTopic, isSub))
        {
            std::cout << "BookKeeping failed for key: " << *(key.value()) << " and destTopic: " << destTopic << std::endl;
            return;
        }
        
        auto& [_, existingFSM] = *it;
        existingFSM->handleEvent(SubUnsubKey(*key),
                        isSub);
        
        std::string group = appGroup + ":" + inTopic + ":" + std::to_string(partition);
        producerFunc(*Topic::pubSub_sync_data(),
            MessageType::sync_data_update(),
            appId,
            json::serialize(json::object{
                {*Tags::group_identifier(), group},
                {*Tags::subscriptionKey(), *(key.value())},
                {*Tags::action(), isSub ?
                    *TagValues::action_subscribe() :
                    *TagValues::action_unsubscribe()},
                {*Tags::destination_topic(), destTopic}
            }),
            {},
            sencCb
        );
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
        std::cout << "Error parsing JSON: " << ec.message() << std::endl;
        return;
    } else {
        std::cout << "Parsed JSON content: " << json::serialize(jsonObj) << std::endl;
    }

    std::cout << "Parsing group from json message";
    std::string group = jsonObj.at(*Tags::group_identifier()).as_string().c_str();
    auto const tokens = group
                | std::views::split(':')
                | std::ranges::to<std::vector<std::string>>();
    
    if (tokens.size() != 3)
    {
        // Log error
        std::cout << "Error: Invalid group identifier format" << std::endl;
        return;
    }

    int32_t partition = atoi((*tokens.rbegin()).c_str());
    std::cout << "Parsed partition from group identifier: " << (*tokens.rbegin()).c_str() << std::endl;

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

std::optional<std::vector<std::string>> decodeKey(std::string key)
{
    auto tokens = key
                | std::views::split(':')
                | std::ranges::to<std::vector<std::string>>();
    if (tokens.size() != 3) {
        return std::nullopt;
    }
    return tokens;
}

void onPriceDataFromExchange(const std::string& key,
                            const std::string& update)
{
    auto const& keyComponents = decodeKey(key);
    if (!keyComponents)    {
        std::cout << "Invalid key format: " << key << std::endl;
        return;
    }

    auto const& priceType = (*keyComponents)[1];

    if (auto it = symbolToDestTopics.find(key); it != symbolToDestTopics.end())
    {
        for (auto const& destTopic : it->second)
        {
            producerFunc(destTopic,
                priceType == *PriceType::depth() ?
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
        std::cout << "No destination topic found for key: " << key << std::endl;
    }
}


void PlatformComm::init(const std::string& brokers,
            const SubUnsubFunc& subFunc,
            const SubUnsubFunc& unsububFunc,
            const KeyGenFunc& keyGenFunc,
            const std::function<void(const DataFunc&)>& registrationFunc,
            const std::shared_ptr<ULMTTools::Timer> timer,
            const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
            const std::string& appId,
            const std::string& appGroup,
            const std::string& inTopic,
            const Middleware::ErrCallback& initErrorCb)
{
    ::appId = appId;
    ::appGroup = appGroup;
    ::inTopic = inTopic;
    ::subFunc = subFunc;
    ::unsubFunc = unsubFunc;
    ::keyGenFunc = keyGenFunc;
    
    registrationFunc(onPriceDataFromExchange);
    
    auto initCb =
    [](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc)
    {
        ::producerFunc = producerFunc;
        ::requestFunc = requestFunc;

        groupConsumerFunc(::inTopic, rebalanceCallback);
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    std::cout << "Initializing middleware" << std::endl;
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
        responseCb
    );
}
