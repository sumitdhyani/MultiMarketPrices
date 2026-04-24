#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <cstdint>
#include <boost/json.hpp>
#include <TypeWrapper.h>
#include <CrazyRouters.hpp>

template <typename... Ts>
struct overload : Ts... { using Ts::operator()...; };

template <class... Ts>
overload(Ts...) -> overload<Ts...>;

// Key derivation: generates a routing key from either a price update or a sub/unsub command
// Defined after SubUnsubVariant below via forward-declared alias

// --- Price update data types (Exchange → Platform) ---
struct TradeUpdate  : TypeWrapper<std::string>{};
struct DepthUpdate  : TypeWrapper<std::string>{};
using MDUpdateVariant = std::variant<TradeUpdate, DepthUpdate>;

// --- Sub/unsub command types (Platform → Exchange) ---
// The wrapped json::object carries the full request so KeyGen can extract the routing key
struct SubRequest   : TypeWrapper<boost::json::object>{};
struct UnsubRequest : TypeWrapper<boost::json::object>{};
using SubUnsubVariant = std::variant<SubRequest, UnsubRequest>;

// Key derivation: generates a routing key from either a price update or a sub/unsub command
using KeyGenFunc = std::function<std::optional<std::string>(const SubUnsubVariant&)>;

// --- Snapshot request types (Platform → Exchange) ---
struct TradeSnapshotRequest : TypeWrapper<std::string>{};  // wraps symbol
struct DepthSnapshotRequest : TypeWrapper<std::string>{};  // wraps symbol
struct InstrumentListRequest{};
using MDReqVariant = std::variant<TradeSnapshotRequest, DepthSnapshotRequest, InstrumentListRequest>;

// --- Response types (Exchange → Platform, on-demand) ---
struct TradeSnapshot    : TypeWrapper<std::string>{};
struct DepthSnapshot    : TypeWrapper<std::string>{};
struct InstrumentRecord : TypeWrapper<std::string>{};
using MDRespVariant = std::variant<TradeSnapshot, DepthSnapshot, InstrumentRecord>;

enum class MDReqType { TradeSnapshot, DepthSnapshot, InstrumentList };

// --- Router instantiations ---
using MDUpdateRouter  = PubSubDataRouter<std::string, uint64_t, MDUpdateVariant>;
using MDControlRouter = PubSubDataRouter<std::string, uint64_t, SubUnsubVariant>;
using MDReqResRouter  = GenericReqRespRouter<uint64_t, MDReqType, uint64_t, MDReqVariant, MDRespVariant>;

// --- Derived callback / listener types ---
using MDUpdateCallback       = MDUpdateRouter::DataCallback;
using MDKeyedUpdateCallback  = MDUpdateRouter::KeyedDataCallback;
using MDControlCallback      = MDControlRouter::DataCallback;
using MDKeyedControlCallback = MDControlRouter::KeyedDataCallback;
using MDResponseHandler  = MDReqResRouter::ResponseHandler;
using MDReqListener      = MDReqResRouter::ReqListener;

// --- PubSub method function types ---
using MDProduceUpdateFunc         = std::function<bool(const std::string&, const MDUpdateVariant&)>;
using MDConsumeUpdateFunc         = std::function<bool(const std::string&, uint64_t, const MDUpdateCallback&)>;
using MDUnregisterUpdateFunc      = std::function<bool(const std::string&, uint64_t)>;
using MDConsumeAllUpdateFunc      = std::function<bool(uint64_t, const MDKeyedUpdateCallback&)>;
using MDUnregisterAllUpdateFunc   = std::function<bool(uint64_t)>;
using MDProduceControlFunc        = std::function<bool(const std::string&, const SubUnsubVariant&)>;
using MDConsumeAllControlFunc     = std::function<bool(uint64_t, const MDKeyedControlCallback&)>;
using MDUnregisterAllControlFunc  = std::function<bool(uint64_t)>;

// --- ReqRes method function types ---
using MDRegisterAsResponderFunc   = std::function<bool(uint64_t, MDReqType, const MDReqListener&)>;
using MDUnregisterAsResponderFunc = std::function<bool(uint64_t, MDReqType)>;
using MDRequestFunc               = std::function<bool(uint64_t, MDReqType, const MDReqVariant&, const MDResponseHandler&)>;

struct MDPubSubMethods
{
    // Exchange → Platform: price update data flow
    const MDProduceUpdateFunc         produceUpdate;
    const MDConsumeUpdateFunc         consumeUpdate;
    const MDUnregisterUpdateFunc      unregisterUpdate;
    const MDConsumeAllUpdateFunc      consumeAllUpdate;
    const MDUnregisterAllUpdateFunc   unregisterFromAllUpdate;

    // Platform → Exchange: subscription command flow
    const MDProduceControlFunc        produceControl;
    const MDConsumeAllControlFunc     consumeAllControl;
    const MDUnregisterAllControlFunc  unregisterFromControl;
};

struct MDReqResMethods
{
    const MDRegisterAsResponderFunc   registerAsResponder;
    const MDUnregisterAsResponderFunc unregisterAsResponder;
    const MDRequestFunc               request;
};

struct MDRoutingMethods
{
    MDPubSubMethods pubSubMethods;
    MDReqResMethods reqResMethods;

    MDRoutingMethods(
        const std::shared_ptr<MDUpdateRouter>&  updateRouter,
        const std::shared_ptr<MDControlRouter>& controlRouter,
        const std::shared_ptr<MDReqResRouter>&  reqResRouter)
    :
        pubSubMethods(
            [updateRouter](const std::string& key, const MDUpdateVariant& data)
                { return updateRouter->produce(key, data); },
            [updateRouter](const std::string& key, uint64_t id, const MDUpdateCallback& cb)
                { return updateRouter->consume(key, id, cb); },
            [updateRouter](const std::string& key, uint64_t id)
                { return updateRouter->unregister(key, id); },
            [updateRouter](uint64_t id, const MDKeyedUpdateCallback& cb)
                { return updateRouter->consumeEveryThing(id, cb); },
            [updateRouter](uint64_t id)
                { return updateRouter->unregisterFromEverything(id); },
            [controlRouter](const std::string& key, const SubUnsubVariant& cmd)
                { return controlRouter->produce(key, cmd); },
            [controlRouter](uint64_t id, const MDKeyedControlCallback& cb)
                { return controlRouter->consumeEveryThing(id, cb); },
            [controlRouter](uint64_t id)
                { return controlRouter->unregisterFromEverything(id); }
        ),
        reqResMethods(
            [reqResRouter](uint64_t responderId, MDReqType reqType, const MDReqListener& listener)
                { return reqResRouter->registerAsResponder(responderId, reqType, listener); },
            [reqResRouter](uint64_t responderId, MDReqType reqType)
                { return reqResRouter->unregisterAsResponder(responderId, reqType); },
            [reqResRouter](uint64_t reqId, MDReqType reqType, const MDReqVariant& data, const MDResponseHandler& handler)
                { return reqResRouter->request(reqId, reqType, data, handler); }
        )
    {}
};
