#pragma once
#include <string>
#include "TypeWrapper.h"


enum class MetaEnum
{
    MessageType,
    Tags,
    HeaderKey,
    TagValues,
    MiddlewareConfig
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

    std::size_t operator()() const
    {
        return std::hash<std::string>{}(this->operator*());
    }

    int operator<=>(const StringEnum& other) const
    {
        return this->operator*().compare(other.operator*());
    }

    private:
    explicit StringEnum(char const* str) : 
        StringWrapper<Enum>(str)
    {}
};

struct MessageType : StringEnum<MetaEnum::MessageType, MessageType>
{
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

// enum class Tags
// {
//     message_type,
//     evt
// };

// enum class HeaderKeys
// {
//     message_type,
//     reqId,
// };


// std::string to_string(const MessageType& msgType )
// {
//     switch (msgType)
//     {
//     case MessageType::subscribe:
//         return "subscribe";
//     case MessageType::unsubscribe:
//         return "unsubscribe";
//     case MessageType::virtual_depth_update:
//         return "virtual_depth_update";
//     case MessageType::depth_update:
//         return "depth_update";
//     case MessageType::trade_update:
//         return "trade_update";
//     case MessageType::request:
//         return "request";
//     case MessageType::response:
//         return "response";
//     case MessageType::last_response:
//         return "last_response";
//     case MessageType::web_server_update:
//         return "web_server_update";
//     case MessageType::sync_data_request:
//         return "sync_data_request";
//     case MessageType::sync_data_update:
//         return "sync_data_update";
//     case MessageType::sync_data:
//         return "sync_data";
//     case MessageType::app_down:
//         return "app_down";
//     case MessageType::app_up:
//         return "app_up";
//     case MessageType::component_subscription_update:
//         return "component_subscription_update";
//     case MessageType::admin_query:
//         return "admin_query";
//     case MessageType::admin_query_response:
//         return "admin_query_response";
//     case MessageType::registration:
//         return "registration";
//     case MessageType::heartBeat:
//         return "heartBeat";
//     case MessageType::component_enquiry:
//         return "component_enquiry";
//     case MessageType::component_subscription:
//         return "component_subscription";
//     case MessageType::component_enquiry_response:
//         return "component_enquiry_response";
//     case MessageType::dummy_message:
//         return "dummy_message";
//     }
// }