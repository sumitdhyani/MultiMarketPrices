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
    std::cout << "Received message from topic: " << topic
            << ", partition: " << partition
            << ", offset: " << offset
            << ", msgType: " << msgType
            << ", key: " << key
            << ", value: " << value
            << ", headers: {";
    for (const auto& [headerKey, headerValue] : headers)
    {
        std::cout << *headerKey << ": " << headerValue << ", ";
    }

    std::cout << "}" << std::endl;
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
    std::cout << "Error while initializing Middleware: " << err.value() << ", " << err.message() << std::endl;
}

void responseCb(const uint64_t& reqId, const std::string& msg, bool isLast)
{
    std::cout   << "Response received:" << std::endl
                << "ReqId: " << reqId << std::endl
                << "Payload: " << msg << std::endl
                << "isLast: " << isLast << std::endl;

    std::error_code ec;
    json::object jsonObj = json::parse(msg, ec).as_object();
    if (ec) {
        std::cerr << "Error parsing JSON: " << ec.message() << std::endl;
    } else {
        std::cout << "Parsed JSON content: " << json::serialize(jsonObj) << std::endl;
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
        
        
        std::cout << "Cleaned params: " << std::endl;
        for(auto const& token: tokens)
        {
            std::cout << token << std::endl;
        }
    }
}



void runningErrorCb(const Middleware::Error& error) {
    std::cout << "[ERROR CALLBACK] Code: " << error.value() 
                << " | Message: " << error.message() 
                << " | Fatal: " << (error.isFatal() ? "YES" : "NO") 
                << std::endl;

    // Connection related errors handle karo
    if (error.value() == RD_KAFKA_RESP_ERR__TRANSPORT ||
        error.value() == RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN ||
        error.value() == RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION) {
        
        std::cout << ">>> Broker Disconnected / Network issue detected! Reconnecting...\n";
    }
    else
    {
        std::cout << "Error from middleware, details: " << error.message() << std::endl;
    }

    if (error.isFatal()) {
        std::cerr << ">>> FATAL ERROR! Application should probably shutdown/restart.\n";
    }
}

void sendCb(const Middleware::RecordMetadata& rm, const Middleware::Error& err) {
    if(!err) return;
    std::cout << "Error while sending msg, code "
                << err.value() << " details: "
                << err.message() << std::endl;
}

int main()
{
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
        std::cout << "Initialization callback called, now subscribing to group consumer topic" << std::endl;
        groupConsumerFunc(*Topic::pubSub_sync_data_requests(), std::nullopt);
    };

    std::string heartBeatStr = getHeartBeatMsg();
    std::string appStr = getDescMsg();

    std::string brokers = "127.0.0.1:9092";
    std::cout << "Initializing middleware" << std::endl;
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
        }
    );
}
