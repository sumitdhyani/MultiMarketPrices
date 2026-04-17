#include <memory>
#include <string.h>
#include <thread>
#include <chrono>
#include "Interface.h"
#include <Logging.h>
#include <kafka/Log.h>

using namespace NanoLog::LogLevels;

// 3rd party library utils
using Worker                = ULMTTools::WorkerThread;
using Timer                 = ULMTTools::Timer;
using TaskScheduler         = ULMTTools::TaskScheduler;
using Properties            = kafka::Properties;
using KafkaProducer         = kafka::clients::producer::KafkaProducer;
using SendOption            = KafkaProducer::SendOption;
using ProducerRecord        = kafka::clients::producer::ProducerRecord;
using KafkaConsumer         = kafka::clients::consumer::KafkaConsumer;
using AdminClient           = kafka::clients::admin::AdminClient;
using SizedBuffer           = kafka::ConstBuffer;
using RebalanceEventType    = kafka::clients::consumer::RebalanceEventType;
using Header                = kafka::Header;

// pair<string, int_32>
using TopicPartition        = kafka::TopicPartition;

// set<TopicPartition>
using TopicPartitions       = kafka::TopicPartitions;

// key reqId, value destination topic
using PendingRequestBook = std::unordered_map<uint64_t, std::string>;
using PendingRequestBook_SPtr = std::shared_ptr<PendingRequestBook>;

namespace Middleware
{

void consumptionThread(KafkaConsumer& consumer,
                        Worker& worker,
                        const MsgCallback& msgCallback,
                        const ErrCallback& errCb)
{
    while(true)
    {
        auto records = consumer.poll(std::chrono::milliseconds(100));
        for (const auto& record : records)
        {
            if(auto err = record.error(); err)
            {
                errCb(err);
                continue;
            }

            const std::string& topic = record.topic();
            const int32_t& partition = record.partition();
            const int64_t& offset = record.offset();
            const std::string& key = record.key().toString();
            const std::string& value = record.value().toString();

            KeyValuePairs headers;
            for (auto const& [key, value] : record.headers())
            {
                headers.emplace(key, value.toString());
            }

            std::string msgType;
            if (headers.find(HeaderKey::message_type()) != headers.end())
            {
                msgType = headers[HeaderKey::message_type()];
            }

            worker.push([topic=std::move(topic),
                        partition,
                        offset,
                        msgType = std::move(msgType),
                        key = std::move(key),
                        headers = std::move(headers),
                        value = std::move(value),
                        msgCallback = msgCallback]()
            {
                msgCallback( topic, partition, offset, msgType, key, headers, value);
            });

        }
    }
}

bool handleResponse(const std::string& topic,
    const int32_t& partition,
    const int64_t& offset,
    const std::string& msgType,
    const std::string& key,
    const Middleware::KeyValuePairs& headers,
    const std::string& msg,
    const ResponseCallback& responseCallback,
    const ErrCallback& errCallback)
{
    if (msgType != *MessageType::response() &&
        msgType != *MessageType::last_response())
    {
        return false;
    }

    uint64_t reqId = 0;
    if (auto it = headers.find(HeaderKey::respId());
        it != headers.end())
    {
        bool isLast = false;
        if (auto it = headers.find(HeaderKey::isLast()); 
            it != headers.end())
        {
            auto const&[_, isLastStr] = *it;
            if (isLastStr != "0" && isLastStr != "1")
            {
                errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Invalid isLast value in response header: " + isLastStr));
                return true;
            }

            isLast = (isLastStr == "1");
        }

        auto const& [_, reqIdStr] = *it;
        try
        {
            reqId = std::stoull(reqIdStr);
            responseCallback(reqId, msg, isLast);
        }
        catch (const std::exception& e)
        {
            errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Invalid reqId in response header: " + reqIdStr + ", error: " + e.what()));
        }
    }
    else
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Missing reqId in response header"));
    }

    return true;
}


bool handleRequest(const std::string& topic,
    const int32_t& partition,
    const int64_t& offset,
    const std::string& msgType,
    const std::string& key,
    const Middleware::KeyValuePairs& headers,
    const std::string& msg,
    const RequestHandlerFunc& requestHandlerFunc,
    const ErrCallback& errCallback,
    const PendingRequestBook_SPtr& pendingRequestBook)
{
    if (msgType != *MessageType::request())
    {
        return false;
    }

    auto it = headers.find(HeaderKey::reqId());
    if (it == headers.end())
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Missing reqId in request header"));
        return true;
    }

    auto const& [_, reqIdStr] = *it;
    uint64_t reqId = 0;
    try
    {
        reqId = std::stoull(reqIdStr);
    }
    catch (const std::exception& e)
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Invalid reqId in request header: " + reqIdStr + ", error: " + e.what()));
        return true;
    }

    if (auto it = pendingRequestBook->find(reqId);
        it != pendingRequestBook->end())
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Duplicate reqId in request header: " + reqIdStr));
        return true;
    }

    it = headers.find(HeaderKey::destTopic());
    if (it == headers.end())
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Missing destTopic in request header"));
        return true;
    }    
    else
    {
        auto const & [_, destTopic] = *it;
        pendingRequestBook->emplace(reqId, destTopic);
    } 

    requestHandlerFunc(reqId, msg);
    return true;
}

APIError respond(const uint64_t& reqId,
    const std::string& respPayload,
    bool isLast,
    const SendCallback& sendCallback,
    const PendingRequestBook_SPtr& pendingRequestBook,
    const ProducerFunc& producerFunc,
    const ErrCallback& errCallback)
{
    if (auto it = pendingRequestBook->find(reqId);
        it == pendingRequestBook->end())
    {
        errCallback(Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Unknown reqId: " + std::to_string(reqId)));
        return APIError::NonExistentReqId;
    }
    else
    {
        auto const& [_, destTopic] = *it;
        producerFunc(destTopic, 
                    MessageType::response(),
                    "Null",
                    respPayload,
                    {{HeaderKey::respId(), std::to_string(reqId)},
                     {HeaderKey::isLast(), isLast ? "1" : "0"}},
                    sendCallback);
        if (isLast) pendingRequestBook->erase(it);
    }

    // Implementation of respond function which sends the response back to the requester
    // It should look up the pendingRequestBook to find the destination topic for the given reqId
    // Then it should produce a message to that topic with the given payload and isLast flag in headers
    // Finally, it should call the sendCallback with appropriate RecordMetadata and Error

    return APIError::Ok;
}

void internalSendCallback(const RecordMetadata& rm, const Error& e)
{

}

void enrichProducerPropsWithErrorCb(Properties& props, ErrCallback errCallback)
{
    props.put("error_cb", errCallback);
    props.put("log_cb", [](int /*level*/, const char* /*filename*/, int /*lineno*/, const char* msg) {
        NANO_LOG(DEBUG, "[Kafka] %s", msg);
    });
}

bool validateInitParams(const std::string& appId,
    const std::string& appGroup,
    const DescriptionFunc& descriptionFunc,
    const HeartBeatGenFunc& heartBeatGenFunc,
    const uint32_t& heartbeatIntervalSec,
    const std::shared_ptr<ULMTTools::Timer>& timer,
    const std::shared_ptr<Worker>& kafkaWorker,
    const MsgCallback& msgCallback,
    const InitCallback& initCallback,
    const ErrCallback& errCallback,
    const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
    const std::unordered_map<MiddlewareConfig, std::string>& consumerProps,
    const ResponseCallback& responseCallback,
    const RequestHandlerFunc& requestHandlerFunc,
    const uint16_t& minAvailableBrokers)
{
    Error error = Error(RD_KAFKA_RESP_ERR_NO_ERROR, "");
    bool ret = false;
    if(appId.empty())                   error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "AppId is empty");
    else if (appGroup.empty())          error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "AppGroup is empty");
    else if (!descriptionFunc)          error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Description function is null");
    else if (!heartBeatGenFunc)         error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "HeartBeatGen function is null");
    else if (heartbeatIntervalSec == 0) error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Heartbeat interval cannot be zero");
    else if (!timer)                    error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Timer is null");
    else if (!kafkaWorker)              error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Worker thread is null");
    else if (!msgCallback)              error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Message callback is null");
    else if (!initCallback)             error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Initialization callback is null");
    else if (!responseCallback)         error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Response callback is None");
    else if (!requestHandlerFunc)       error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Request handler function is None");
    else if (!minAvailableBrokers)      error = Error(RD_KAFKA_RESP_ERR_UNKNOWN, "Minimum available brokers cannot be zero");
    else                                ret = true;


    /******************************************************************************************************/
    errCallback(error);
    return ret;
}

void initializeMiddleWare(const std::string& appId,
    const std::string& appGroup,
    const DescriptionFunc& descriptionFunc,
    const HeartBeatGenFunc& heartBeatGenFunc,
    const uint32_t& heartbeatIntervalSec,
    const std::shared_ptr<ULMTTools::Timer>& timer,
    const std::shared_ptr<Worker>& kafkaWorker,
    const MsgCallback& msgCallback,
    const InitCallback& initCallback,
    const ErrCallback& errCallback,
    const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
    const std::unordered_map<MiddlewareConfig, std::string>& consumerProps,
    const ResponseCallback& responseCallback,
    const RequestHandlerFunc& requestHandlerFunc,
    uint16_t minAvailableBrokers)
{
    // Redirect Kafka library-level logs to NanoLog
    kafka::setGlobalLogger([](int /*level*/, const char* /*filename*/, int /*lineno*/, const char* msg) {
        NANO_LOG(DEBUG, "[Kafka] %s", msg);
    });

    if (!validateInitParams(appId,
            appGroup,
            descriptionFunc,
            heartBeatGenFunc,
            heartbeatIntervalSec,
            timer,
            kafkaWorker,
            msgCallback,
            initCallback,
            errCallback,
            producerProps,
            consumerProps,
            responseCallback,
            requestHandlerFunc,
            minAvailableBrokers))
    {
        return;
    }

    std::shared_ptr<KafkaProducer> producer;
    std::shared_ptr<KafkaConsumer> groupConsumer;
    std::shared_ptr<KafkaConsumer> individualConsumer;

    try
    {
        Properties kafkaProducerProps;
        for (auto const& [key, value] : producerProps)
        {
            kafkaProducerProps.put(*key, value);
        }
        enrichProducerPropsWithErrorCb(kafkaProducerProps, errCallback);
        producer = std::make_shared<KafkaProducer>(kafkaProducerProps);

        Properties kafkaConsumerProps;
        for (auto const& [key, value] : consumerProps)
        {
            kafkaConsumerProps.put(*key, value);    
        }
        enrichProducerPropsWithErrorCb(kafkaConsumerProps, errCallback);

        Properties adminProps;
        adminProps.put(*MiddlewareConfig::bootstrap_servers(),
                        producerProps.at(MiddlewareConfig::bootstrap_servers()));

        AdminClient adminClient(adminProps);
        if(auto res = adminClient.createTopics({appId}, 1, minAvailableBrokers);
           res.error && res.error.value() != RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS)
        {
            throw res.error;
        }

        kafkaConsumerProps.put(*MiddlewareConfig::group_id(), appGroup);
        groupConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);

        kafkaConsumerProps.put(*MiddlewareConfig::group_id(), appId);
        individualConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);
    }
    catch(const Error& e)
    {
        errCallback(e);
        return;
    }
    catch(const std::exception& e)
    {
        //Error error(RD_KAFKA_RESP_ERR_UNKNOWN, e.what());
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN);
        errCallback(error);
        return;
    }
    catch(...)
    {
        //Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Unknown error during middleware initialization");
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Unknown error during middleware initialization");
        errCallback(error);
        return;
    }

    ProducerFunc producerFunc =
    [producer, kafkaWorker](const std::string&   topic,
            const MessageType&   msgType,
            const std::string&   key,
            const std::string&   payload,
            const KeyValuePairs& headers,
            const SendCallback&   sendCallback)
    {
        if (topic.empty())          return APIError::TopicEmpty;
        else if (msgType->empty())  return APIError::MsgTypeEmpty;
        else if (key.empty())       return APIError::KeyEmpty;
        else if (payload.empty())   return APIError::PayloadEmpty;

        auto msgType_copy = std::make_shared<std::string>(*msgType);
        auto key_copy = std::make_shared<std::string>(key);
        
        ProducerRecord record(topic,
            SizedBuffer(key_copy->c_str(), key_copy->length()),
            SizedBuffer(payload.c_str(), payload.length()));

        record.headers().emplace_back(*HeaderKey::message_type(), Header::Value{msgType_copy->c_str(), msgType_copy->length()});
        std::vector<std::shared_ptr<std::string>> headerValueCopies; // To ensure the lifetime of header values
        for (const auto& [headerKey, headerValue] : headers)
        {
            auto headerValueCopy = std::make_shared<std::string>(headerValue);
            headerValueCopies.push_back(headerValueCopy);
            record.headers().emplace_back(*headerKey, Header::Value{headerValueCopy->c_str(), headerValueCopy->length()});
        }
        
        producer->send(record,
            [sendCallback,
            kafkaWorker,
            msgType_copy,
            key_copy,
            headerValueCopies = std::move(headerValueCopies)]
            (const RecordMetadata& metadata, const Error& error) {
                if (!sendCallback) return;
                kafkaWorker->push([sendCallback, metadata, error]() {
                    sendCallback(metadata, error);
                });
            },
            SendOption::ToCopyRecordValue
        );

        return APIError::Ok;
    };

    LowLevelProducerFunc lowLevelProducerFunc =
    [producer, kafkaWorker](const char*   topic,
            const char*   msgType,
            const char*   key,
            const char*   payload,
            const uint32_t&      payloadSize,   
            const LowLevelKeyValuePairs& headers,
            const std::vector<uint32_t>& headerSizes,
            const SendCallback&   sendCallback)
    {
        if (!topic)             return APIError::TopicEmpty;
        else if (!msgType)      return APIError::MsgTypeEmpty;
        else if (!key)          return APIError::KeyEmpty;
        else if (!payload)      return APIError::PayloadEmpty;

        
        ProducerRecord record(topic,
            SizedBuffer(key, strlen(key)),
            SizedBuffer(payload, payloadSize));

        record.headers().emplace_back(*HeaderKey::message_type(), Header::Value{msgType, strlen(msgType)});
        for (size_t i = 0; i < headers.size(); ++i)
        {
            const auto& [headerKey, headerValue] = headers[i];
            record.headers().emplace_back(headerKey, Header::Value{headerValue, headerSizes[i]});
        }
        
        producer->send(record,
            [sendCallback, kafkaWorker](const RecordMetadata& metadata, const Error& error) {
                if (!sendCallback) return;
                kafkaWorker->push([sendCallback, metadata, error]() {
                    sendCallback(metadata, error);
                });
            },
            SendOption::ToCopyRecordValue
        );

        return APIError::Ok;
    };

    auto getSubsciptionFunc =
    [errCallback](const std::shared_ptr<KafkaConsumer>& consumer)
    {
        return 
        [consumer, errCallback](const std::string& topic, const std::optional<RebalanceCallback>& rebalanceCallback)
        {
            if (topic.empty()) return APIError::TopicEmpty;
            
            if (!rebalanceCallback) {
                consumer->subscribe({topic});
                return APIError::Ok;
            }

            try
            {
                consumer->subscribe(
                    {topic}, 
                    [rebalanceCallback](
                        const RebalanceEventType et,
                        const TopicPartitions& tps ) 
                    {
                        TopicAssignmentEvent assignmentEvent =
                            (et == RebalanceEventType::PartitionsAssigned) ?
                            TopicAssignmentEvent::PartitionAssigned :
                            TopicAssignmentEvent::PartitionRevoked;

                        for(auto const& [topic, partition] : tps)
                        {
                            (*rebalanceCallback)(assignmentEvent, topic, partition);
                        }
                    }
                );
            }
            catch(Error& e)
            {
                errCallback(e);
                return APIError::SubscriptionFailed;
            }
            return APIError::Ok;
        };
    };

    ConsumerFunc groupConsumerFunc = getSubsciptionFunc(groupConsumer);
    ConsumerFunc individualConsumerFunc = getSubsciptionFunc(individualConsumer);

    auto pendingRequestBook = std::make_shared<PendingRequestBook>();

    MsgCallback refinedMsgCallback =
    [errCallback, responseCallback, requestHandlerFunc, msgCallback, pendingRequestBook]
    (const std::string& topic,
        const int32_t& partition,
        const int64_t& offset,
        const std::string& msgType,
        const std::string& key,
        const KeyValuePairs& headers,
        const std::string& payload)
    {

        if (!handleResponse(topic, partition, offset, msgType, key, headers, payload, responseCallback, errCallback)
            && !handleRequest(topic, partition, offset, msgType, key, headers, payload, requestHandlerFunc, errCallback, pendingRequestBook))
        {
            msgCallback(topic, partition, offset, msgType, key, headers, payload);
        }
        // Here we can do some common processing for all messages before passing to user defined callback, e.g. logging, metrics, etc.
    };



    RequestFunc requestFunc =
    [producerFunc, appId](const uint64_t& reqId,
        const std::string& payload,
        const std::string& targetTopic,
        const SendCallback& sendCallback)
    {
        producerFunc(targetTopic,
                     MessageType::request(),
                    appId,
                    payload,
                    {{HeaderKey::reqId(), std::to_string(reqId)},
                     {HeaderKey::destTopic(), appId}},
                    sendCallback);
        return APIError::Ok;
    };

    std::thread gcThread([groupConsumer, kafkaWorker, refinedMsgCallback, errCallback]() {
        consumptionThread(*groupConsumer, *kafkaWorker, refinedMsgCallback, errCallback);
    });
        
    timer->install([producerFunc, errCallback, appId, heartBeatGenFunc]() {
        producerFunc(*Topic::heartbeats(),
                    MessageType::heartBeat(),
                    appId,
                    heartBeatGenFunc(),
                    {},
                    internalSendCallback);
    }, std::chrono::seconds(heartbeatIntervalSec));

    auto respondFunc =
    [producerFunc, errCallback, pendingRequestBook]
    (const uint64_t& reqId,  // ReqId
        const std::string& respPayload,         // Resp payload
        bool isLast,                       // isLast
        const SendCallback& sendCallback)
    {
        return respond(reqId, respPayload, isLast, sendCallback, pendingRequestBook, producerFunc, errCallback);
    };

    initCallback(producerFunc, lowLevelProducerFunc, groupConsumerFunc, individualConsumerFunc, requestFunc,  respondFunc);

    while(individualConsumerFunc(appId, std::nullopt) != APIError::Ok);
    
    consumptionThread(*individualConsumer, *kafkaWorker, refinedMsgCallback, errCallback);
}

}