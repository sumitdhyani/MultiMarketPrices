#pragma once
#include <string>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <exception>
#include <tuple>
#include <stdint.h>
#include <MTTools/Timer.hpp>
#include <MTTools/WorkerThread.hpp>
#include <kafka/Error.h>
#include <kafka/KafkaProducer.h>
#include <kafka/KafkaConsumer.h>
#include <Constants.h>


namespace Middleware
{
    using RecordMetadata            = kafka::clients::producer::RecordMetadata;
    using Error                     = kafka::Error;
    using ErrCallback               = std::function<void(const Error&)>;
    using SendCallback              = std::function<void(const RecordMetadata&, const Error&)>;
    using KeyValuePairs             = std::unordered_map<HeaderKey,std::string>;

    using LowLevelKeyValuePairs     = std::vector<std::tuple<const char*,const char*>>;

    using ProducerFunc              = std::function<APIError (const std::string&,   // Topic
                                                    const MessageType&,   // MessageType
                                                    const std::string&,   // Key
                                                    const std::string&,   // Message payload
                                                    const KeyValuePairs&, // Headers
                                                    const SendCallback&)>; // Sucess callback
    

    // No memory management done 
    using LowLevelProducerFunc     = std::function<APIError (const char*,   // Topic
                                                            const char*,   // MessageType
                                                            const char*,   // Key
                                                            const char*,   // Message payload
                                                            const uint32_t&,      // Message payload size
                                                            const LowLevelKeyValuePairs&, // Headers
                                                            const std::vector<uint32_t>&, // Header Sizes
                                                            const SendCallback&)>; // Sucess callback

    using DescriptionFunc   = std::function<std::string()>;
    using HeartBeatGenFunc  = DescriptionFunc;

    using MsgCallback = std::function<void (const std::string&,     // Topic
                                            const int32_t&,         // Patition
                                            const int64_t&,         // offset
                                            const std::string&,     // MsgType
                                            const std::string&,     // Key
                                            const KeyValuePairs&,   // Headers
                                            const std::string&)>;   // Message payload

    using RebalanceCallback = std::function<void (const TopicAssignmentEvent&,
                                                const std::string&,
                                                const int32_t&)>;

    using ConsumerFunc = std::function<APIError (const std::string&,
                                                const RebalanceCallback& rebalanceCallback)>;

    using InitCallback = std::function<void(const ProducerFunc&,
                                            const LowLevelProducerFunc&,
                                            const ConsumerFunc&,    // To subscribe as group
                                            const ConsumerFunc&)>;  // To subscribe as individual

    void initializeMiddleWare(const std::string& appId,
                            const std::string& appType,
                            const DescriptionFunc& descriptionFunc,
                            const HeartBeatGenFunc& heartBeatGenFunc,
                            const uint32_t& heartbeatIntervalSec,
                            const std::shared_ptr<ULMTTools::Timer>& timer,
                            const std::shared_ptr<ULMTTools::WorkerThread>& workerThread,
                            const MsgCallback& msgCallback,
                            const InitCallback& initCallback,
                            const ErrCallback& errCallback,
                            const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
                            const std::unordered_map<MiddlewareConfig, std::string>& consumerProps);
}