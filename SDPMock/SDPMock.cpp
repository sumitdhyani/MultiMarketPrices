#include <iostream>
#include <chrono>
#include <ranges>
#include <system_error>
#include <stdint.h>
#include <boost/json.hpp>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <MiddleWare/Interface.h>
#include <Constants.h>
#include <Logging.h>

using namespace NanoLog::LogLevels;

using Timer = ULMTTools::Timer;
namespace json = boost::json;
std::string appId = "DummyDumper_1";
std::string appGroup = "DummyDumper_Group";

Middleware::ProducerFunc producerFunc;
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


void initErrorCb(const Middleware::Error& err)
{
    NANO_LOG(DEBUG, "Error while initializing Middleware: %d, %s", err.value(), err.message().c_str());
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    NANO_LOG(DEBUG, "Response received: ReqId: %lu, Payload: %s, isLast: %d", reqId, msg.c_str(), isLast);

    std::error_code ec;
    json::object jsonObj = json::parse(msg, ec).as_object();
    if (ec) {
        NANO_LOG(DEBUG, "Error parsing JSON: %s", ec.message().c_str());
    } else {
        NANO_LOG(DEBUG, "Parsed JSON content: %s", json::serialize(jsonObj).c_str());
    }

    if(!isLast)
    {
        std::string params = jsonObj["key"].as_string().c_str();
        std::erase_if(params, [](const char c) {
            return c == '\'' || c == '(' || c == '#' || c == ')' || c == ' ';
        });

        auto tokens = params
                | std::views::split(',')
                | std::ranges::to<std::vector<std::string>>();
        
        
        NANO_LOG(DEBUG, "Cleaned params:");
        for(auto const& token: tokens)
        {
            NANO_LOG(DEBUG, "%s", token.c_str());
        }
    }
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

void sendCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    NANO_LOG(DEBUG, "Error while sending msg, code %d details: %s", err.value(), err.message().c_str());
}

int main()
{
    Logging::init("SDPMock");

    auto scheduler = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer =  std::make_shared<ULMTTools::Timer>(scheduler);
    auto worker = std::make_shared<ULMTTools::WorkerThread>();
    
    auto initCb =
    [timer, worker](const Middleware::ProducerFunc& producerFunc,
            const Middleware::LowLevelProducerFunc& lowLevelProducerFunc,
            const Middleware::ConsumerFunc& groupConsumerFunc,
            const Middleware::ConsumerFunc& individualConsumerFunc,
            const Middleware::RequestFunc& requestFunc,
            const Middleware::RespondFunc& respondFunc)
    {
        ::producerFunc = producerFunc;
        ::requestFunc = requestFunc;
        ::respondFunc = respondFunc;
        NANO_LOG(DEBUG, "Initialization callback called, now subscribing to group consumer topic");
        groupConsumerFunc(*Topic::pubSub_sync_data_requests(), std::nullopt);
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    std::string brokers = "127.0.0.1:9092";
    NANO_LOG(DEBUG, "Initializing middleware");
    Middleware::initializeMiddleWare("SDPMock_1",
        "SDPMock",
        [appStr]() { return appStr; },
        [heartBeatStr]() { return heartBeatStr; },
        5,
        timer,
        worker,
        msgCb,
        initCb,
        initErrorCb,
        {
            {MiddlewareConfig::bootstrap_servers(), brokers},
        },
        {
            {MiddlewareConfig::bootstrap_servers(), brokers},
            {MiddlewareConfig::group_id(), "DataDumperApp"}
        },
        responseCb,
        [](const uint64_t& reqId, const std::string& reqpayload){
            auto const obj = json::parse(reqpayload).as_object();
            const std::string group = obj.at(*Tags::group_identifier()).as_string().c_str();
            respondFunc(
                reqId,
                json::serialize(json::object{
                    {*Tags::group_identifier(), group},
                    {*Tags::message_type(), *MessageType::response()}}),
                true,
                sendCb);
        },
        1
    );
}
