#include <boost/json/object.hpp>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdint.h>
#include <boost/json.hpp>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>
#include <UUIDGen.hpp>
#include <Logging.h>
#include <ConfigLib/ConfigLib.h>

using namespace NanoLog::LogLevels;

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";

Middleware::ProducerFunc producerFunc;
Middleware::ConsumerFunc groupConsumerFunc;
Middleware::ConsumerFunc individualConsumerFunc;
Middleware::RequestFunc requestFunc;
Middleware::RespondFunc respondFunc;

std::string getHeartBeatMsg()
{
    return json::serialize(json::object({
        {*Tags::message_type(),*MessageType::heartBeat()},
        {*Tags::HeartBeatInterval(),5},
        {*Tags::HeartBeatTimeout(),30},
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
}

void msgCb(const std::string& topic,
    const int32_t& partition,
    const int64_t& offset,
    const std::string& msgType,
    const std::string& key,
    const Middleware::KeyValuePairs& headers,
    const std::string& value)
{
    // Process the received message (for demonstration, we just print it)
    NANO_LOG(DEBUG, "Received message from topic: %s, partition: %d, offset: %ld, msgType: %s, key: %s, value: %s",
            topic.c_str(), partition, offset, msgType.c_str(), key.c_str(), value.c_str());
    for (const auto& [headerKey, headerValue] : headers)
    {
        NANO_LOG(DEBUG, "Header - %s: %s", (*headerKey).c_str(), headerValue.c_str());
    }
}

std::string getDescMsg()
{
    return json::serialize(json::object({
        {*Tags::appId(),appId},
        {*Tags::appGroup(), appGroup}
    }));
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    NANO_LOG(DEBUG, "Response received: ReqId: %lu, Payload: %s, isLast: %d", reqId, msg.c_str(), isLast);
}

void sencCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    NANO_LOG(DEBUG, "Error while sending msg, code %d details: %s", err.value(), err.message().c_str());
}

void mainLoop()
{
  static UUIDGenerator gen;
  while (true) {
    std::cout << "Enter the target app to ping" << std::endl;
    std::string targetApp;
    std::getline(std::cin, targetApp);
    uint64_t reqId = gen.generate64();
    requestFunc(reqId, ReqType::ping(), "", targetApp, sencCb);
    std::cout << "Ent ping, reqId: " << reqId << std::endl; 
  }
}

void initErrorCb(const Middleware::Error& err)
{
    NANO_LOG(DEBUG, "Error while initializing Middleware: %d, %s", err.value(), err.message().c_str());
}

void runningErrorCb(const Middleware::Error& error) {
    NANO_LOG(DEBUG, "[ERROR CALLBACK] Code: %d | Message: %s | Fatal: %s",
            error.value(), error.message().c_str(),
            error.isFatal() ? "YES" : "NO");

    if (error.value() == RD_KAFKA_RESP_ERR__TRANSPORT ||
        error.value() == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN ||
        error.value() == RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION) {
        
        NANO_LOG(DEBUG, ">>> Broker Disconnected / Network issue detected! Reconnecting...");
    }
    else
    {
        NANO_LOG(DEBUG, "Error from middleware, details: %s", error.message().c_str());
    }

    if (error.isFatal()) {
        NANO_LOG(DEBUG, ">>> FATAL ERROR! Application should probably shutdown/restart.");
    }
}

bool validateConfig(const json::object& cfg)
{
    return true;
}

void onConfigUpdate(const json::object& cfg)
{
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <appId>" << std::endl;
        return 1;
    }
    const std::string appId = argv[1];

    auto const& cfg_opt = Config::init(appId, std::nullopt, validateConfig);
    if (!cfg_opt)
    {
        return 1;
    }
    auto cfg = *cfg_opt;
    auto scheduler = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer =  std::make_shared<ULMTTools::Timer>(scheduler);
    
    auto initCb =
    [timer](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc,
            const Middleware::RespondFunc& respondFunc,
            const Middleware::ShutdownFunc& /*shutdownFunc*/)
    {
        ::producerFunc = producerFunc;
        ::groupConsumerFunc = groupConsumerFunc;
        ::individualConsumerFunc = individualConsumerFunc;
        ::requestFunc = requestFunc;
        ::respondFunc = respondFunc;

        std::thread(mainLoop).detach();
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    const std::string brokers = cfg.at(*ConfigTag::brokers()).as_string().c_str();

    NANO_LOG(DEBUG, "Initializing middleware");
    Middleware::initializeMiddleWare(
        appId, *AppGroup::DataDumper(), [appStr]() { return appStr; },
        [heartBeatStr]() { return heartBeatStr; }, 5, timer,
        std::make_shared<ULMTTools::WorkerThread>(), msgCb, initCb, initErrorCb,
        {
            {MiddlewareConfig::bootstrap_servers(), brokers},
        },
        {{MiddlewareConfig::bootstrap_servers(), brokers}}, responseCb,
        [](const uint64_t &, const std::string &) {
          NANO_LOG(DEBUG, "Received request without a handler");
        },
        [](const uint64_t &reqId) { 
            std::cout << "Pong callback received, reqId: " << reqId << std::endl;
        },
        cfg.at(*ConfigTag::numMinBrokers()).as_int64(),
        cfg.at(*ConfigTag::heartbeatsTopic()).as_string().c_str(),
        cfg.at(*ConfigTag::registrationsTopic()).as_string().c_str(),
        std::nullopt);
}
