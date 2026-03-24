#pragma once
#include <string>
#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <exception>
#include <stdint.h>
#include <kafka/Error.h>
#include "Constants.h"

using error  = std::tuple<int, std::string>;
using ErrCallback = std::function<void(const error&)>;

using KeyValuePairs = std::unordered_map<HeaderKey,std::string>;
using ProducerFunc = std::function<void (const std::string&,    // Topic
                                         const MessageType&,    // MessageType
                                         const std::string&,    // Key
                                         const std::string&,    // Message payload
                                         const KeyValuePairs&,  // Headers
                                         const ErrCallback&)>;  // Sucess callback

using DescriptionFunc = std::function<std::string()>;

using MsgCallback = std::function<void (const std::string&,     // Topic
                                        const uint64_t&,        // Patition
                                        const uint64_t&,        // offset
                                        const std::string&,     // MsgType
                                        const std::string&,     // Key
                                        const KeyValuePairs&,   // Headers
                                        const std::string&)>;   // Message payload

using ConsumerFunc = std::function<void (const std::string& topic)>;

using InitCallback = std::function<void(const ProducerFunc&,
                                        const ConsumerFunc&)>;

void initializeMiddleWare(const MsgCallback&,
                          const DescriptionFunc&,
                          const InitCallback&,
                          const ErrCallback&,
                          const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
                          const std::unordered_map<MiddlewareConfig, std::string>& consumerProps);