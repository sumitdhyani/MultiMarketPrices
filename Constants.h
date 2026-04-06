#pragma once
#include <string>
#include <optional>
#include "TypeWrapper.h"

#define NUllKey "Null"

enum class MetaEnum
{
    MessageType,
    Tags,
    HeaderKey,
    TagValues,
    MiddlewareConfig,
    Topic,
    InstrumentType,
    InstrumentAttributes,
    OptionType,
    PriceType
};

template<MetaEnum Enum>
using StringWrapper = TypeWrapper<std::string>;


template<MetaEnum Enum, class SubClass>
struct StringEnum : StringWrapper<Enum>
{
    friend SubClass;
    using StringWrapper<Enum>::StringWrapper;

    StringEnum(const StringEnum& other) :
        StringWrapper<Enum>(static_cast<const StringWrapper<Enum>&>(other))    
    {}
    
    StringEnum(StringEnum&& other) :
        StringWrapper<Enum>(static_cast<StringWrapper<Enum>&&>(other))    
    {}

    bool operator==(const StringEnum& other) const
    {
        return this->operator*() == other.operator*();
    }

    private:
    explicit StringEnum(char const* str) : 
        StringWrapper<Enum>(str)
    {}
};

struct MessageType : StringEnum<MetaEnum::MessageType, MessageType>
{
    static MessageType const& dummy_message()
    { 
        static MessageType instance{"dummy_message"};
        return instance;
    }

    static MessageType const& heartBeat()
    { 
        static MessageType instance{"heartBeat"};
        return instance;
    }
    
    static MessageType const& subscribe()
    { 
        static MessageType instance{"subscribe"};
        return instance;
    }
    
    static MessageType const& unsubscribe()
    { 
        static MessageType instance{"unsubscribe"};
        return instance;
    }
    
    static MessageType const& virtual_depth_update()
    { 
        static MessageType instance{"virtual_depth_update"};
        return instance;
    }

    static MessageType const& depth_update()
    { 
        static MessageType instance{"depth_update"};
        return instance;
    }

    static MessageType const& trade_update()
    { 
        static MessageType instance{"trade_update"};
        return instance;
    }

    static MessageType const& request()
    { 
        static MessageType instance{"request"};
        return instance;
    }

    static MessageType const& response()
    { 
        static MessageType instance{"response"};
        return instance;
    }

    static MessageType const& last_response()
    { 
        static MessageType instance{"last_response"};
        return instance;
    }

    static MessageType const& web_server_update()
    { 
        static MessageType instance{"web_server_update"};
        return instance;
    }

    static MessageType const& sync_data_request()
    { 
        static MessageType instance{"sync_data_request"};
        return instance;
    }

    static MessageType const& sync_data_update()
    { 
        static MessageType instance{"sync_data_update"};
        return instance;
    }

    static MessageType const& sync_data()
    { 
        static MessageType instance{"sync_data"};
        return instance;
    }

    static MessageType const& app_down()
    { 
        static MessageType instance{"app_down"};
        return instance;
    }

    static MessageType const& app_up()
    { 
        static MessageType instance{"app_up"};
        return instance;
    }

    static MessageType const& component_subscription_update()
    { 
        static MessageType instance{"component_subscription_update"};
        return instance;
    }

    static MessageType const& admin_query()
    { 
        static MessageType instance{"admin_query"};
        return instance;
    }
    
    static MessageType const& admin_query_response()
    { 
        static MessageType instance{"admin_query_response"};
        return instance;
    }

    static MessageType const& registration()
    { 
        static MessageType instance{"registration"};
        return instance;
    }

    static MessageType const& component_enquiry()
    { 
        static MessageType instance{"component_enquiry"};
        return instance;
    }

    static MessageType const& component_subscription()
    { 
        static MessageType instance{"component_subscription"};
        return instance;
    }

    static MessageType const& component_enquiry_response()
    { 
        static MessageType instance{"component_enquiry_response"};
        return instance;
    }
};



struct PriceType : StringEnum<MetaEnum::PriceType, PriceType>
{
    static PriceType const& depth()
    {
        static PriceType instance{"depth"};
        return instance;    
    }

    static PriceType const& trade()
    {
        static PriceType instance{"trade"};
        return instance;    
    }
};

struct InstrumentType : StringEnum<MetaEnum::InstrumentType, InstrumentType>
{
    static InstrumentType const& spot()
    {
        static InstrumentType instance{"spot"};
        return instance;    
    }

    static InstrumentType const& future()
    {
        static InstrumentType instance{"future"};
        return instance;    
    }

    static InstrumentType const& option()
    {
        static InstrumentType instance{"option"};
        return instance;    
    }
};

struct OptionType : StringEnum<MetaEnum::OptionType, OptionType>
{
    static OptionType const& call()
    {
        static OptionType instance{"call"};
        return instance;    
    }

    static OptionType const& put()
    {
        static OptionType instance{"put"};
        return instance;    
    }
};

struct InstrumentAttributes : StringEnum<MetaEnum::InstrumentAttributes, InstrumentAttributes>
{
    static InstrumentAttributes const& instrument_type()
    {
        static InstrumentAttributes instance{"instrument_type"};
        return instance;
    }

    static InstrumentAttributes const& option_type()
    {
        static InstrumentAttributes instance{"option_type"};
        return instance;
    }
};

struct Tags : StringEnum<MetaEnum::Tags, Tags>
{
    static Tags const& message_type()
    { 
        static Tags instance{"message_type"};
        return instance;
    }
    
    static Tags const& evt()
    { 
        static Tags instance{"evt"};
        return instance;
    }

    static Tags const& HeartBeatInterval()
    { 
        static Tags instance{"HeartBeatInterval"};
        return instance;
    }

    static Tags const& HeartBeatTimeout()
    { 
        static Tags instance{"HeartBeatTimeout"};
        return instance;
    }

    static Tags const& appId()
    { 
        static Tags instance{"appId"};
        return instance;
    }

    static Tags const& appGroup()
    { 
        static Tags instance{"appGroup"};
        return instance;
    }

    static Tags const& latestOffSet()
    { 
        static Tags instance{"latestOffSet"};
        return instance;
    }

    static Tags const& subscriptionKey()
    { 
        static Tags instance{"key"};
        return instance;
    }

    static Tags const& symbol()
    { 
        static Tags instance{"symbol"};
        return instance;
    }

    static Tags const& action()
    { 
        static Tags instance{"action"};
        return instance;
    }

    static Tags const& subscription_type()
    { 
        static Tags instance{"type"};
        return instance;
    }

    static Tags const& group_identifier()
    { 
        static Tags instance{"group"};
        return instance;
    }

    static Tags const& destination_topic()
    { 
        static Tags instance{"destination_topic"};
        return instance;
    }

    static Tags const& destination_topics()
    { 
        static Tags instance{"destination_topics"};
        return instance;
    }

    static Tags const& exchange()
    { 
        static Tags instance{"exchange"};
        return instance;
    }
};

struct TagValues : StringEnum<MetaEnum::TagValues, Tags>
{
    static TagValues const& action_subscribe()
    { 
        static TagValues instance{"subscribe"};
        return instance;
    }

    static TagValues const& action_unsubscribe()
    { 
        static TagValues instance{"unsubscribe"};
        return instance;
    }

    static Tags const& subscription_type_depth()
    { 
        static Tags instance{"depth"};
        return instance;
    }

    static Tags const& subscription_type_trade()
    { 
        static Tags instance{"trade"};
        return instance;
    }
};

struct HeaderKey : StringEnum<MetaEnum::HeaderKey, HeaderKey>
{
    static HeaderKey const& message_type()
    { 
        static HeaderKey instance{"message_type"};
        return instance;
    }
    
    static HeaderKey const& reqId()
    { 
        static HeaderKey instance{"reqId"};
        return instance;
    }

    static HeaderKey const& respId()
    { 
        static HeaderKey instance{"respId"};
        return instance;
    }

    static HeaderKey const& destTopic()
    { 
        static HeaderKey instance{"destTopic"};
        return instance;
    }

    static HeaderKey const& response_error()
    { 
        static HeaderKey instance{"response_error"};
        return instance;
    }

    static HeaderKey const& isLast()
    { 
        static HeaderKey instance{"isLast"};
        return instance;
    }
};

struct MiddlewareConfig : StringEnum<MetaEnum::MiddlewareConfig, MiddlewareConfig>
{
    static MiddlewareConfig const& bootstrap_servers()
    { 
        static MiddlewareConfig instance{"bootstrap.servers"};
        return instance;
    }
    
    static MiddlewareConfig const& group_id()
    { 
        static MiddlewareConfig instance{"group.id"};
        return instance;
    }
    
    static MiddlewareConfig const& enable_auto_commit()
    { 
        static MiddlewareConfig instance{"enable.auto.commit"};
        return instance;
    }
    
    static MiddlewareConfig const& auto_commit_interval_ms()
    { 
        static MiddlewareConfig instance{"auto.commit.interval.ms"};
        return instance;
    }
    
    static MiddlewareConfig const& session_timeout_ms()
    { 
        static MiddlewareConfig instance{"session.timeout.ms"};
        return instance;
    }
    
    static MiddlewareConfig const& auto_offset_reset()
    { 
        static MiddlewareConfig instance{"auto.offset.reset"};
        return instance;
    }
};

struct Topic : StringEnum<MetaEnum::Topic, Topic>
{
    static Topic const& heartbeats()
    { 
        static Topic instance{"heartbeats"};
        return instance;
    }

    static Topic const& prices()
    { 
        static Topic instance{"prices"};
        return instance;
    }

    static Topic const& test_topic()
    { 
        static Topic instance{"test_topic"};
        return instance;
    }

    // Used to query the subscription state of a group
    // gropu format: "<gateway_type>:<input_topic>:<partition>"
    static Topic const& pubSub_sync_data_requests()
    { 
        static Topic instance{"pubSub_sync_data_requests"};
        return instance;
    }

    static Topic const& pubSub_sync_data()
    { 
        static Topic instance{"pubSub_sync_data"};
        return instance;
    }
};

namespace std {
    template<>
    struct hash<HeaderKey> {
        std::size_t operator()(const HeaderKey& k) const noexcept {
           return std::hash<std::string>{}(*k);
        }
    };

    template<>
    struct hash<MiddlewareConfig> {
        std::size_t operator()(const MiddlewareConfig& k) const noexcept {
           return std::hash<std::string>{}(*k);
        }
    };

    // template<MetaEnum Enum, class SubClass>
    // struct hash<StringEnum<Enum, SubClass>> {
    //     std::size_t operator()(const StringEnum<Enum, SubClass>& k) const noexcept {
    //        return std::hash<std::string>{}(*k);
    //     }
    // };

}

enum class TopicAssignmentEvent
{
    PartitionAssigned,
    PartitionRevoked
};

enum class APIError
{
    Ok,
    TopicEmpty,
    MsgTypeEmpty,
    KeyEmpty,
    PayloadEmpty,
    SubscriptionFailed,
    InvalidInstrumentType,
    NonExistentReqId
};

inline std::optional<PriceType> strToPriceType(const std::string& priceType)
{
    if (priceType == *TagValues::subscription_type_depth()) return PriceType::depth();
    else if (priceType == *TagValues::subscription_type_trade()) return PriceType::trade();
    else return std::nullopt;
}