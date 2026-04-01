#pragma once
#include <iostream>
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
#include "PerPartitionSM.h"

namespace json = boost::json;
using PubSubFunc = std::function<void(const std::string&, const PriceType&)>;
using DataFunc = std::function<void(const std::string&,
                                    const PriceType&,
                                    const std::string&)>;

namespace PlatformComm
{
    void init(const std::string& brokers,
            const PubSubFunc& subFunc,
            const PubSubFunc& unsububFunc,
            const std::function<void(const DataFunc&)>& registrationFunc,
            const std::shared_ptr<ULMTTools::Timer> timer,
            const std::shared_ptr<ULMTTools::WorkerThread> workerThread,
            const std::string& appId,
            const std::string& appGroup,
            const std::string& inTopic,
            const Middleware::ErrCallback& initErrorCb);
}