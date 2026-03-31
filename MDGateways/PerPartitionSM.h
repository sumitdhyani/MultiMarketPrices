#pragma once
#include <functional>
#include <ranges>
#include <tuple>
#include <iostream>
#include <CPPFsm/FSM.hpp>
#include "SMEvents.h"

using State = ULFSM::State;
using SpecialTransition = ULFSM::Specialtransition;
using Transition = ULFSM::Transition;

template <class... EvtTypes>
using IEventProcessor = ULFSM::IEventProcessor<EvtTypes...>;

template <typename Derived>
using FSM = ULFSM::FSM<Derived>;
using SubUnsubFunc = std::function<void(const std::string&, const std::string&)>;

// to be called when a sub/unsub call is actually made
using ActionNotificationFunc =
std::function<void(const Instrument&,              // 
    const SubscriptionType&,        // 
    const bool&)>;                  // true: Sub, false: Unsub

struct Downloading : State,
IEventProcessor<SubscriptionRecord>,
IEventProcessor<DownloadEnd>,
IEventProcessor<Instrument, SubscriptionType, bool>,
IEventProcessor<Revoke>
{
    private:
    const SubUnsubFunc m_subFunc;
    const SubUnsubFunc m_unsubFunc;
    const int32_t m_partition;

    const ActionNotificationFunc f_actionNotificationFunc;

    public:
    Downloading(const int32_t& partition,
        const SubUnsubFunc& subFunc,
        const SubUnsubFunc& unsubFunc,
        const ActionNotificationFunc& actionNotificationFunc) : 
        State(false),
        m_partition(partition),
        m_subFunc(subFunc),
        m_unsubFunc(unsubFunc),
        f_actionNotificationFunc(actionNotificationFunc)
    {}

    const std::optional<std::tuple<std::string,std::string>> getSubscriptionInfo(std::string& params)
    {
        std::optional<std::tuple<std::string,std::string>> res;
        // At the moment, key is to be of the format "('<instrumentId>', '<SubscriptionType>')"
        std::erase_if(params, [](const char c) {
            return c == '\'' || c == '(' || c == ')' || c == ' ';
        });

        auto tokens = params
                | std::views::split(',')
                | std::ranges::to<std::vector<std::string>>();

        if(tokens.size() == 2) {
            res = std::make_tuple(tokens[0], tokens[1]);
        }
        return res;
    }
    
    /********************************Event Handlers ****************************************************** */
    
    Transition process(const SubscriptionRecord& subscription) override
    {
        auto const& dict = *subscription;
        std::string subscriptionKey = dict.at(*Tags::subscriptionKey()).as_string().c_str();
        auto params = std::move(getSubscriptionInfo(subscriptionKey));
        if(params) {
            auto& [instrument, subscriptionType] = *params;
            m_subFunc(instrument.c_str(), subscriptionType.c_str());
        }
        return SpecialTransition::nullTransition;
    }

    Transition process(const Instrument& instrument,
                        const SubscriptionType& subscriptionType,
                        const bool& subscribe) override
    {
        return SpecialTransition::deferralTransition;
    };

    Transition process(const Revoke&) override
    {
        return SpecialTransition::deferralTransition;
    };

    // Defined in cpp file to avoid circular dependency with the events header
    Transition process(const DownloadEnd& downloadEnd) override;
};


struct Operational : State,
IEventProcessor<Instrument, SubscriptionType, bool>,
IEventProcessor<Revoke>
{
    private:
    const int32_t m_partition;
    const SubUnsubFunc m_subFunc;
    const SubUnsubFunc m_unsubFunc;
    const ActionNotificationFunc m_actionNotificationFunc;

    public:
    Operational(const int32_t& partition,
                const SubUnsubFunc& subFunc,
                const SubUnsubFunc& unsubFunc,
                const ActionNotificationFunc& actionNotificationFunc) : 
        State(false), m_partition(partition),
        m_subFunc(subFunc),
        m_unsubFunc(unsubFunc),
        m_actionNotificationFunc(actionNotificationFunc)
    {}

    /********************************Event Handlers ****************************************************** */

    Transition process(const Instrument& instrument, 
                        const SubscriptionType& subscriptionType,
                        const bool& subscribe) override
    {
        subscribe?
            m_subFunc(*instrument, *subscriptionType):
            m_unsubFunc(*instrument, *subscriptionType);

        m_actionNotificationFunc(instrument, subscriptionType, subscribe);

        return SpecialTransition::nullTransition;
    }

    // Defined in cpp file to avoid circular dependency with the events header
    Transition process(const Revoke& revoke) override;
};

struct Revoked : State,
IEventProcessor<Assign>
{
    private:
    const int32_t m_partition;

    const SubUnsubFunc f_subFunc;
    const SubUnsubFunc f_unsubFunc;
    const ActionNotificationFunc f_actionNotificationFunc;
    
    public:
    Revoked(int32_t partition,
            const SubUnsubFunc& subFunc,
            const SubUnsubFunc& unsubFunc,
            const ActionNotificationFunc& actionNotificationFunc) :
        State(false),
        m_partition(partition),
        f_subFunc(subFunc),
        f_unsubFunc(unsubFunc),
        f_actionNotificationFunc(actionNotificationFunc) {}

    /********************************Event Handlers ****************************************************** */
    
    // Defined in cpp file to avoid circular dependency with the events header
    Transition process(const Assign&) override;
};


// Responsibilities:
// Forward sub-unsub requests to the provided api, dumb way, just hand over to theh api
struct PerPartitionFSM : FSM<PerPartitionFSM>
{
    PerPartitionFSM(const int32_t& partition,
        const SubUnsubFunc& subFunc,
        const SubUnsubFunc& unsubFunc,
        const ActionNotificationFunc& actionNotificationFunc) :
        FSM<PerPartitionFSM>([&]() {
             return std::make_unique<Downloading>(partition, subFunc, unsubFunc, actionNotificationFunc); 
        })
    {}
};