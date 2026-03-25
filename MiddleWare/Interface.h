#pragma once
#include <string>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <exception>
#include <optional>
#include <stdint.h>
#include "Constants.h"

using error             = std::tuple<int, std::string>;
using ErrCallback       = std::function<void(const error&)>;
using KeyValuePairs     = std::unordered_map<HeaderKey,std::string>;

using ProducerFunc      = std::function<APIError (const std::string&,   // Topic
                                                  const MessageType&,   // MessageType
                                                  const std::string&,   // Key
                                                  const std::string&,   // Message payload
                                                  const KeyValuePairs&, // Headers
                                                  const ErrCallback&)>; // Sucess callback

using DescriptionFunc = std::function<std::string()>;

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
                                        const ConsumerFunc&,    // To subscribe as group
                                        const ConsumerFunc&)>;  // To subscribe as individual

void initializeMiddleWare(const std::string& appId,
                          const std::string& appType,
                          const DescriptionFunc& descriptionFunc,
                          const MsgCallback& msgCallback,
                          const InitCallback& initCallback,
                          const ErrCallback& errCallback,
                          const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
                          const std::unordered_map<MiddlewareConfig, std::string>& consumerProps);