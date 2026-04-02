#include "PlatformComm.h"

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId;
std::string appGroup;
std::string inTopic;
PubSubFunc subFunc;
PubSubFunc unsubFunc;

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
            json::object obj = json::parse(msg).as_object();
            Instrument instrument(obj.at(*Tags::symbol()).as_string().c_str());
            SubscriptionType subType(obj.at(*Tags::subscription_type()).as_string().c_str());
            bool isSub = msgType == *MessageType::subscribe();
            std::string destTopic = obj.at(*Tags::destination_topic()).as_string().c_str();

            auto& [_, existingFSM] = *it;
            existingFSM->handleEvent(instrument,
                            subType,
                            isSub);

            std::string key = *instrument + "," + *subType;
            std::string group = appGroup + ":" + inTopic + ":" + std::to_string(partition);
            producerFunc(*Topic::pubSub_sync_data(),
                MessageType::sync_data_update(),
                appId,
                json::serialize(json::object{
                    {*Tags::group_identifier(), group},
                    {*Tags::subscriptionKey(), key},
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

void onPriceDataFromExchange(const std::string& symbol,
                            const PriceType& priceType,
                            const std::string& update)
{
    producerFunc(*Topic::prices(),
                priceType == PriceType::depth() ?
                    MessageType::depth_update() :
                    MessageType::trade_update(),
                symbol,
                update, 
                {}, 
                sencCb);
}


void PlatformComm::init(const std::string& brokers,
            const PubSubFunc& subFunc,
            const PubSubFunc& unsububFunc,
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
