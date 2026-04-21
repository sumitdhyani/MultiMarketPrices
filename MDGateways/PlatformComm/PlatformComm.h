#pragma once
#include <iostream>
#include <string>
#include <chrono>
#include <optional>
#include <ranges>
#include <system_error>
#include <stdint.h>
#include <boost/json.hpp>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>
#include <UUIDGen.hpp>
#include <TypeWrapper.h>
#include <unordered_map>
#include <MDGatewayRouterConfig.h>

namespace json = boost::json;

// Format:
// <instrumentId>:<priceType>:<InstrumentType>:<OptionType>
// option type should only be there if instrument type is option
struct SubUnsubKey : TypeWrapper<std::string> {};
using SubUnsubFunc = std::function<void(const std::string&)>;
using SnapshotCallback = std::function<void(const boost::json::object&, const std::string&)>;

using SymbolStore = std::unordered_map<std::string, std::string>;
using IntrumentListCallback = std::function<void(const SymbolStore&)>;
using GetInstrumentListFunc = std::function<void(const IntrumentListCallback&)>;
using GetPriceSnapshotFunc = std::function<void(const std::string&, const PriceType&, const SnapshotCallback&)>;

namespace PlatformComm
{
    void init(const std::shared_ptr<MDRoutingMethods>& routing,
            const KeyGenFunc& keyGenFunc,
            const std::string& brokers,
            const std::shared_ptr<ULMTTools::Timer> timer,
            const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
            const std::string& appId,
            const std::string& appGroup,
            const std::string& inTopic,
            const Middleware::ErrCallback& initErrorCb,
            const uint16_t& minAvailableBrokers);
}