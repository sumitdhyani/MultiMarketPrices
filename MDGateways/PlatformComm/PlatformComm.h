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

namespace json = boost::json;

// Fomat:
// <instrumentId>:<priceType>:<InstrumentType>:<OptionType>
// option type should only be there if instrument type is option
struct SubUnsubKey : TypeWrapper<std::string> {};
using SubUnsubFunc = std::function<void(const SubUnsubKey&)>;
using DataFunc = std::function<void(const std::string&, // Key
                                    const std::string&)>; // Update 



namespace PlatformComm
{
    void init(const std::string& brokers,
            const SubUnsubFunc& subFunc,
            const SubUnsubFunc& unsububFunc,
            const std::function<void(const DataFunc&)>& registrationFunc,
            const std::shared_ptr<ULMTTools::Timer> timer,
            const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
            const std::string& appId,
            const std::string& appGroup,
            const std::string& inTopic,
            const Middleware::ErrCallback& initErrorCb);
}