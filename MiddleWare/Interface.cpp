#include <memory>
#include <string.h>
#include <thread>
#include <chrono>
#include "Interface.h"


// 3rd party library utils
using Worker                = ULMTTools::WorkerThread;
using Timer                 = ULMTTools::Timer;
using TaskScheduler         = ULMTTools::TaskScheduler;
using Properties            = kafka::Properties;
using KafkaProducer         = kafka::clients::producer::KafkaProducer;
using ProducerRecord        = kafka::clients::producer::ProducerRecord;
using KafkaConsumer         = kafka::clients::consumer::KafkaConsumer;
using SizedBuffer           = kafka::ConstBuffer;
using RebalanceEventType    = kafka::clients::consumer::RebalanceEventType;
using Header                = kafka::Header;

// pair<string, int_32>
using TopicPartition        = kafka::TopicPartition;

// set<TopicPartition>
using TopicPartitions       = kafka::TopicPartitions;


namespace Middleware
{

void consumptionThread(KafkaConsumer& consumer,
                    Worker& worker,
                    const MsgCallback& msgCallback)
{
    using namespace Middleware;
    while(true)
    {
        auto records = consumer.poll(std::chrono::milliseconds(100));
        for (const auto& record : records)
        {
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


void internalSendCallback(const RecordMetadata& rm, const Error& e)
{

}

void enrichProducerPropsWithErrorCb(Properties& props, ErrCallback errCallback)
{
    props.put("error_cb", errCallback);
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
    const std::unordered_map<MiddlewareConfig, std::string>& consumerProps)
{
    if(appId.empty())
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "AppId is empty");
        errCallback(error);
        return false;
    }
    else if (appGroup.empty())
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "AppGroup is empty");
        errCallback(error);
        return false;
    }
    else if(!descriptionFunc)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Description function is null");
        errCallback(error);
        return false;
    }
    else if(!heartBeatGenFunc)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "HeartBeatGen function is null");
        errCallback(error);
        return false;
    }
    else if(heartbeatIntervalSec == 0)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Heartbeat interval cannot be zero");
        errCallback(error);
        return false;
    }
    else if (!timer)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Timer is null");
        errCallback(error);
        return false;
    }
    else if (!kafkaWorker)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Worker thread is null");
        errCallback(error);
        return false;
    }
    else if (!msgCallback)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Message callback is null");
        errCallback(error);
        return false;
    }
    else if (!initCallback)
    {
        Error error(RD_KAFKA_RESP_ERR_UNKNOWN, "Initialization callback is null");
        errCallback(error);
        return false;
    }

    return true;
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
    const std::unordered_map<MiddlewareConfig, std::string>& consumerProps)
{
    if (!validateInitParams(appId, appGroup, descriptionFunc, heartBeatGenFunc, heartbeatIntervalSec, timer, kafkaWorker, msgCallback, initCallback, errCallback, producerProps, consumerProps))
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
        auto producer = std::make_shared<KafkaProducer>(kafkaProducerProps);

        Properties kafkaConsumerProps;
        for (auto const& [key, value] : consumerProps)
        {
            kafkaConsumerProps.put(*key, value);    
        }
        enrichProducerPropsWithErrorCb(kafkaConsumerProps, errCallback);

        kafkaConsumerProps.put(*MiddlewareConfig::group_id(), appGroup);
        auto groupConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);

        kafkaConsumerProps.put(*MiddlewareConfig::group_id(), appId);
        auto individualConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);
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
        auto payload_copy = std::make_shared<std::string>(payload);
        
        ProducerRecord record(topic,
            SizedBuffer(key_copy->c_str(), key_copy->length()),
            SizedBuffer(payload_copy->c_str(), payload_copy->length()));

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
            payload_copy,
            headerValueCopies = std::move(headerValueCopies)]
            (const RecordMetadata& metadata, const Error& error) {
                if (!sendCallback) return;
                kafkaWorker->push([sendCallback, metadata, error]() {
                    sendCallback(metadata, error);
                });
            }
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
            }
        );

        return APIError::Ok;
    };

    auto getSubsciptionFunc =
    [](const std::shared_ptr<KafkaConsumer>& consumer)
    {
        return 
        [consumer](const std::string& topic, const RebalanceCallback& rebalanceCallback)
        {
            if (topic.empty()) return APIError::TopicEmpty;
            
            if (!rebalanceCallback) {
                consumer->subscribe({topic});
                return APIError::Ok;
            }

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
                        rebalanceCallback(assignmentEvent, topic, partition);
                    }
                }
            );
            return APIError::Ok;
        };
    };

    ConsumerFunc groupConsumerFunc = getSubsciptionFunc(groupConsumer);
    ConsumerFunc individualConsumerFunc = getSubsciptionFunc(individualConsumer);


    std::thread gcThread([groupConsumer, kafkaWorker, msgCallback]() {
        consumptionThread(*groupConsumer, *kafkaWorker, msgCallback);
    });
        
    std::thread icThread([individualConsumer, kafkaWorker, msgCallback]() {
        consumptionThread(*individualConsumer, *kafkaWorker, msgCallback);
    });

    timer->install([producerFunc, errCallback, appId]() {
        producerFunc(*Topic::heartbeats(),
                    MessageType::heartBeat(),
                    appId,
                    "", 
                    {},
                    internalSendCallback);
    }, std::chrono::seconds(heartbeatIntervalSec));

    initCallback(producerFunc, lowLevelProducerFunc, groupConsumerFunc, individualConsumerFunc);
}

}