#include "Interface.h"
#include <kafka/Error.h>
#include <kafka/KafkaProducer.h>
#include <kafka/KafkaConsumer.h>


void initializeMiddleWare(const MsgCallback& msgCallback,
                          const DescriptionFunc& descriptionFunc,
                          const InitCallback& initCallback,
                          const ErrCallback& errCallback,
                          const std::unordered_map<MiddlewareConfig, std::string>& producerProps,
                          const std::unordered_map<MiddlewareConfig, std::string>& consumerProps)
{
    using namespace kafka;
    using namespace kafka::clients;
    using namespace kafka::clients::producer;

    try
    {
        Properties kafkaProducerProps;
        for (auto const& [key, value] : producerProps)
        {
            kafkaProducerProps.put(*key, value);    
        }

        KafkaProducer producer(kafkaProducerProps);

        Properties kafkaConsumerProps;
        for (auto const& [key, value] : consumerProps)
        {
            kafkaConsumerProps.put(*key, value);    
        }

    }
    catch(const std::out_of_range& e)
    {
        errCallback({-1, e.what()});
    }
    catch(const std::exception& e)
    {
        errCallback({-1, e.what()});
    }
    catch(const kafka::Error& e)
    {
        errCallback({e.value(), e.message()});
    }
    catch(...)
    {
        errCallback({-1, "Unknown error during middleware initialization"});
    }
}