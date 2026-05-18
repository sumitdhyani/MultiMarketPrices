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

static const std::string INSTRUMENT_WILD_CARD = "*";

// Gateway operational status — platform-level states meaningful to all consumers.
// Gateway-specific nuance is communicated via the free-text `detail` field.
// NOTE: intentionally scoped to PlatformComm; these states are only valid for
//       MD gateways.  Process lifecycle (up/down) is handled by app_up/app_down.
struct GatewayStatus : StringEnum<MetaEnum::GatewayStatusEnum, GatewayStatus>
{
    static GatewayStatus const& init()
    {
        static GatewayStatus instance{"init"};
        return instance;
    }
    static GatewayStatus const& operational()
    {
        static GatewayStatus instance{"operational"};
        return instance;
    }
    static GatewayStatus const& degraded()
    {
        static GatewayStatus instance{"degraded"};
        return instance;
    }
    static GatewayStatus const& disconnected()
    {
        static GatewayStatus instance{"disconnected"};
        return instance;
    }
};

using PublishStatusFunc = std::function<void(const GatewayStatus&, const std::string&)>;
using GatewayInitCb    = std::function<void(const PublishStatusFunc&)>;

namespace PlatformComm
{
void init(const std::shared_ptr<MDRoutingMethods> &routing,
    const KeyGenFunc &keyGenFunc,
    const KeyDisintegrationFunc &keyDisintegrationFunc,
    const std::string &brokers,
    const std::shared_ptr<ULMTTools::Timer> timer,
    const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
    const std::string &appId, const std::string &appGroup,
    const std::string &inTopic,
    const Middleware::ErrCallback &initErrorCb,
    const uint16_t &minAvailableBrokers,
    const std::string& heartbeatTopic,
    const std::string& registrationsTopic,
    const std::string& syncDataTopic,
    const std::string& syncDataRequestTopic,
    const std::string& statusTopic,
    const GatewayInitCb& gatewayInitCb,
    const std::unordered_map<std::string, std::string>& extraKafkaProps,
    const std::function<void()>& exitCb);
}