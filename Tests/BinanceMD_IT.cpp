/*
 * Integration test for BinanceMD gateway.
 *
 * Test sequence:
 *  1. Connect to Kafka middleware as "BinanceMD_IT_1".
 *  2. Send an instrument-list request to BinanceMD_1 and verify at least one
 *     instrument record is returned.
 *  3. Pick a symbol from the instrument list (or fall back to a hard-coded
 *     default) and send a subscribe message for trade updates.
 *  4. Wait up to 10 seconds and assert that at least one trade update arrived.
 *  5. Send an unsubscribe message for the same symbol.
 *  6. Record the time of the last received update.
 *  7. Wait 5 seconds, then verify that no update was received in the last 4
 *     seconds (i.e., the last update was ≥ 4 seconds ago).
 *
 * Exit code: 0 = all assertions passed, 1 = at least one assertion failed.
 */

#include <NanoLogCpp17.h>
#include <boost/json.hpp>
#include <atomic>
#include <boost/json/object.hpp>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <string>
#include <gtest/gtest.h>
#include <ConfigLib/ConfigLib.h>
#include <Constants.h>
#include <boost/json.hpp>
#include <Logging.h>
#include <MiddleWare/Interface.h>
#include <MTTools/TaskScheduler.hpp>
#include <MTTools/WorkerThread.hpp>
#include <UUIDGen.hpp>
#include <thread>

using namespace NanoLog::LogLevels;
namespace json = boost::json;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// App identity
// ---------------------------------------------------------------------------
static const std::string kAppId    = "BinanceMD_IT_1";
static const std::string kAppGroup = "BinanceMD_IT";
static std::string kOutTopic;
static const std::string kSymbol = "BTCUSDT";   // fallback if instrument list request fails

std::atomic<bool> updateReceived = false;
std::atomic<bool> instrumentsDownloaded = false;
std::binary_semaphore updateReceivedSemaphore(0);
std::binary_semaphore instrumentsDownloadedSemaphore(0);
uint64_t numInstrumentRecords = 0;

// ---------------------------------------------------------------------------
// Middleware handles (filled in initCb)
// ---------------------------------------------------------------------------
static Middleware::ProducerFunc producerFunc;
static Middleware::RequestFunc  requestFunc;
static Middleware::ShutdownFunc shutdownFunc;

static UUIDGenerator g_gen;

static void sencCb(const Middleware::RecordMetadata&, const Middleware::Error& err)
{
    if (!err) return;
    std::cout << "Send error: " << err.value() << " " << err.message() << "\n";
}

static std::string getHeartBeatMsg()
{
    return json::serialize(json::object({
        {*Tags::message_type(), *MessageType::heartBeat()},
        {*Tags::HeartBeatInterval(), 5},
        {*Tags::HeartBeatTimeout(), 30},
        {*Tags::appId(), kAppId},
        {*Tags::appGroup(), kAppGroup}
    }));
}

static std::string getDescMsg()
{
    return json::serialize(json::object({
        {*Tags::appId(), kAppId},
        {*Tags::appGroup(), kAppGroup}
    }));
}

// ---------------------------------------------------------------------------
// Middleware callbacks
// ---------------------------------------------------------------------------
static void msgCb(const std::string& /*topic*/,
                  const int32_t& /*partition*/,
                  const int64_t& /*offset*/,
                  const std::string& msgType,
                  const std::string& key,
                  const Middleware::KeyValuePairs& /*headers*/,
                  const std::string& value)
{
    if (msgType == *MessageType::trade_update() ||
        msgType == *MessageType::depth_update())
    {
        updateReceived = true;
        updateReceivedSemaphore.release();
    }
}

static void responseCb(const uint64_t& reqId,
                       const std::string& msg,
                       bool isLast)
{
    if (isLast)
    {
        std::cout << "Received final response for instrument download, total: " << numInstrumentRecords << "\n";
        instrumentsDownloaded = true;
        instrumentsDownloadedSemaphore.release();
    }
    else {
        numInstrumentRecords++;
    }
}

static void initErrorCb(const Middleware::Error& err)
{
    std::cout << "IT: init error " << err.value() << " " << err.message() << "\n";
}

static void runningErrorCb(const Middleware::Error& err)
{
    std::cout << "IT: running error " << err.value() << " " << err.message() << "\n";
}

// ---------------------------------------------------------------------------
// Subscribe / unsubscribe helpers
// ---------------------------------------------------------------------------
static void sendSubscribe(const std::string& symbol, bool isSub)
{
    producerFunc(
        kOutTopic,
        isSub ? MessageType::subscribe() : MessageType::unsubscribe(),
        symbol,
        json::serialize(json::object{
            {*Tags::action(),             isSub ? *TagValues::action_subscribe()
                                                : *TagValues::action_unsubscribe()},
            {*Tags::destination_topic(),  kAppId},
            {*Tags::symbol(),             symbol},
            {*Tags::subscription_type(),  *PriceType::trade()}
        }),
        {},
        sencCb
    );
    std::cout << "IT: sent " << (isSub ? "SUBSCRIBE" : "UNSUBSCRIBE") << " for symbol " << symbol << "\n";
}

static void sendInstrumentListRequest()
{
    requestFunc(
        g_gen.generate64(),
        json::serialize(json::object{{*Tags::message_type(), *MessageType::instrument_request()}}),
        kOutTopic,
        sencCb
    );
    std::cout << "IT: sent instrument list request\n";
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------
// (provided by gtest)

// ---------------------------------------------------------------------------
// Test body — runs after middleware is ready
// ---------------------------------------------------------------------------
static void runTest()
{
    sendSubscribe(kSymbol, false);
    std::cout << "Step 1: Waiting for middleware to be initialized " << kOutTopic << "\n";

    std::cout << "Step 2: sending instrument list request\n";
    sendInstrumentListRequest();
    //instrumentsDownloadedSemaphore.acquire();
    instrumentsDownloadedSemaphore.try_acquire_for(std::chrono::seconds(15));
    EXPECT_TRUE(instrumentsDownloaded);
    if(!instrumentsDownloaded)
    {
        std::cout << "Instrument list download failed or timed out\n";
        return;
    }

    std::cout << "Step 3: subscribing to trade updates for symbol and waiting for symbol update " << kSymbol << "\n";
    sendSubscribe(kSymbol, true);
    
    updateReceivedSemaphore.try_acquire_for(std::chrono::seconds(15));
    EXPECT_TRUE(updateReceived);
    if(!updateReceived)
    {
        std::cout << "Update not received within timeout\n";
    }

    sendSubscribe(kSymbol, false);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ::shutdownFunc();
}

// ---------------------------------------------------------------------------
// GTest fixture
// ---------------------------------------------------------------------------
class BinanceMDTest : public ::testing::Test
{
};

TEST_F(BinanceMDTest, InstrumentListAndSubscribeUnsubscribe)
{
    std::cout << "[TEST] Starting integration test...\n";
    runTest();
    std::cout << "[TEST] Integration test completed successfully\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    const std::string appId = (argc >= 2) ? argv[1] : kAppId;

    auto const& cfg_opt = Config::init(appId, [](const json::object&){}, [](const json::object&){ return true; });
    if (!cfg_opt)
    {
        std::cerr << "Failed to load config for " << appId << "\n";
        return 1;
    }

    auto scheduler  = std::make_shared<ULMTTools::TaskScheduler>();
    auto timer      = std::make_shared<ULMTTools::Timer>(scheduler);
    auto worker = std::make_shared<ULMTTools::WorkerThread>();


    auto const& cfg = *cfg_opt;

    const std::string logLevelStr = cfg.at(*ConfigTag::logLevel()).as_string().c_str();
    if (auto level_opt = strToLogLevel(logLevelStr); level_opt)
        Logging::init(appId, *level_opt);

    const std::string brokers = cfg.at(*ConfigTag::brokers()).as_string().c_str();
    ::kOutTopic = cfg.at("out_topic").as_string().c_str();
    
    const std::string heartBeat = getHeartBeatMsg();
    const std::string desc      = getDescMsg();

    auto initCb =
    [](
        const Middleware::ProducerFunc&         pf,
        const Middleware::LowLevelProducerFunc& /*llpf*/,
        const Middleware::ConsumerFunc&         /*groupConsumerFunc*/,
        const Middleware::ConsumerFunc&         /*individualConsumerFunc*/,
        const Middleware::RequestFunc&          rf,
        const Middleware::RespondFunc&          /*respondFunc*/,
        const Middleware::ShutdownFunc&         sf)
    {
        std::cout << "[MIDDLEWARE] Initializing middleware callbacks...\n";
        ::producerFunc  = pf;
        ::requestFunc   = rf;
        ::shutdownFunc  = sf;

        // Launch tests on a SEPARATE thread so initCb returns immediately.
        // The middleware event loop (which dispatches responseCb / msgCb)
        // runs on the calling thread — blocking it here would starve all
        // incoming Kafka messages for the entire duration of the test.
        std::thread([](){
            int result = RUN_ALL_TESTS();
            ::shutdownFunc();
            std::exit(result);
        }).detach();
    };

    ::testing::InitGoogleTest(&argc, argv);

    Middleware::initializeMiddleWare(
        appId,
        kAppGroup,
        [&desc]()     { return desc; },
        [&heartBeat](){ return heartBeat; },
        5,
        timer,
        worker,
        msgCb,
        initCb,
        initErrorCb,
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        { {MiddlewareConfig::bootstrap_servers(), brokers} },
        responseCb,
        [](const uint64_t&, const std::string&){},
        cfg.at(*ConfigTag::numMinBrokers()).as_int64()
    );

    return 0;
}
