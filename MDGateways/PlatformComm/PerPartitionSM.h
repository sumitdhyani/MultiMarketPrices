#pragma once
#include <functional>
#include <ranges>
#include <tuple>
#include <iostream>
#include <CPPFsm/FSM.hpp>
#include <iostream>
#include "PlatformComm.h"
#include "SMEvents.h"

using State = ULFSM::State;
using SpecialTransition = ULFSM::Specialtransition;
using Transition = ULFSM::Transition;

template <class... EvtTypes>
using IEventProcessor = ULFSM::IEventProcessor<EvtTypes...>;

template <typename Derived>
using FSM = ULFSM::FSM<Derived>;

struct Downloading : State,
IEventProcessor<SubscriptionRecord>,
IEventProcessor<DownloadEnd>,
IEventProcessor<SubUnsubKey, bool>,
IEventProcessor<Revoke>
{
    private:
    const SubUnsubFunc m_subFunc;
    const SubUnsubFunc f_unsubFunc;
    const int32_t m_partition;

    public:
    Downloading(const int32_t& partition,
        const SubUnsubFunc& subFunc,
        const SubUnsubFunc& unsubFunc) : 
        State(false),
        m_partition(partition),
        m_subFunc(subFunc),
        f_unsubFunc(unsubFunc)
    {}

    /********************************Event Handlers ****************************************************** */
    
    Transition process(const SubscriptionRecord& subscription) override
    {
        auto const& dict = *subscription;
        std::string subscriptionKey = dict.at(*Tags::subscriptionKey()).as_string().c_str();
        m_subFunc(subscriptionKey);
        return SpecialTransition::nullTransition;
    }

    Transition process(const SubUnsubKey& key, const bool& subscribe) override
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
IEventProcessor<SubUnsubKey, bool>,
IEventProcessor<Revoke>
{
    private:
    const int32_t m_partition;
    const SubUnsubFunc m_subFunc;
    const SubUnsubFunc m_unsubFunc;

    public:
    Operational(const int32_t& partition,
                const SubUnsubFunc& subFunc,
                const SubUnsubFunc& unsubFunc) : 
        State(false), m_partition(partition),
        m_subFunc(subFunc),
        m_unsubFunc(unsubFunc)
    {}

    /********************************Event Handlers ****************************************************** */

    Transition process(const SubUnsubKey& key,
                        const bool& subscribe) override
    {
        std::cout << "Operational State: Received sub/unsub event for key: " << *key 
                << ", action: " << (subscribe? "subscribe" : "unsubscribe") 
                << std::endl;
        

        subscribe?
            m_subFunc(*key):
            m_unsubFunc(*key);

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
    
    public:
    Revoked(int32_t partition,
            const SubUnsubFunc& subFunc,
            const SubUnsubFunc& unsubFunc) :
        State(false),
        m_partition(partition),
        f_subFunc(subFunc),
        f_unsubFunc(unsubFunc)
    {}

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
        const SubUnsubFunc& unsubFunc) :
        FSM<PerPartitionFSM>([&]() {
             return std::make_unique<Downloading>(partition, subFunc, unsubFunc); 
        })
    {}
};