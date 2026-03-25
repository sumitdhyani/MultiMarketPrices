#include "Interface.h"
#include <memory>
#include <thread>
#include <chrono>
#include <kafka/Error.h>
#include <kafka/KafkaProducer.h>
#include <kafka/KafkaConsumer.h>
#include <WorkerThread.hpp>


// 3rd party library utils
using Worker                = ULMTTools::WorkerThread;
using Error                 = kafka::Error;
using Properties            = kafka::Properties;
using KafkaProducer         = kafka::clients::producer::KafkaProducer;
using RecordMetadata        = kafka::clients::producer::RecordMetadata;
using ProducerRecord        = kafka::clients::producer::ProducerRecord;
using KafkaConsumer         = kafka::clients::consumer::KafkaConsumer;
using SizedBuffer           = kafka::ConstBuffer;
using RebalanceEventType    = kafka::clients::consumer::RebalanceEventType;
using Header                = kafka::Header;

// pair<string, int_32>
using TopicPartition        = kafka::TopicPartition;

// set<TopicPartition>
using TopicPartitions       = kafka::TopicPartitions;

void consumptionThread(KafkaConsumer& consumer,
                       Worker& worker,
                       const Middleware::MsgCallback& msgCallback)
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

void Middleware::initializeMiddleWare(const std::string& appId,
    const std::string& appGroup,
    const DescriptionFunc& descriptionFunc,
    const MsgCallback& msgCallback,
    const InitCallback& initCallback,
    const ErrCallback& errCallback,
    const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
    const std::unordered_map<MiddlewareConfig, std::string>& consumerProps)
{
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

        producer = std::make_shared<KafkaProducer>(kafkaProducerProps);

        Properties kafkaConsumerProps;
        for (auto const& [key, value] : consumerProps)
        {
            kafkaConsumerProps.put(*key, value);    
        }

        kafkaConsumerProps.put(MiddlewareConfig::group_id().operator*(), appGroup);
        groupConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);

        kafkaConsumerProps.put(MiddlewareConfig::group_id().operator*(), appId);
        individualConsumer = std::make_shared<KafkaConsumer>(kafkaConsumerProps);
    }
    catch(const kafka::Error& e)
    {
        errCallback({e.value(), e.message()});
    }
    catch(const std::exception& e)
    {
        errCallback({-1, e.what()});
    }
    catch(...)
    {
        errCallback({-1, "Unknown error during middleware initialization"});
    }

    ProducerFunc producerFunc =
    [producer](const std::string&   topic,
               const MessageType&   msgType,
               const std::string&   key,
               const std::string&   payload,
               const KeyValuePairs& headers,
               const ErrCallback&   errCallback)
    {
        if (topic.empty())          return APIError::TopicEmpty;
        else if (msgType->empty())   return APIError::MsgTypeEmpty;
        else if (key.empty())       return APIError::KeyEmpty;
        else if (payload.empty())   return APIError::PayloadEmpty;

        ProducerRecord record(topic,
            SizedBuffer(key.c_str(), key.length()),
            SizedBuffer(payload.c_str(), payload.length()));

        record.headers().emplace_back(*HeaderKey::message_type(), Header::Value{msgType->c_str(), msgType->length()});
        for (const auto& [headerKey, headerValue] : headers)
        {
            record.headers().emplace_back(*headerKey, Header::Value{headerValue.c_str(), headerValue.length()});
        }
        
        producer->send(record,
            [errCallback](const RecordMetadata& metadata, const Error& error) {
                errCallback({error.value(), error.message()});
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

    initCallback(producerFunc, groupConsumerFunc, individualConsumerFunc);

    auto consumptionWorker = std::make_shared<Worker>();

    std::thread gcThread([groupConsumer, consumptionWorker, msgCallback]() {
        consumptionThread(*groupConsumer, *consumptionWorker, msgCallback);
    });
        
    std::thread icThread([individualConsumer, consumptionWorker, msgCallback]() {
        consumptionThread(*individualConsumer, *consumptionWorker, msgCallback);
    });


}