#pragma once
#include <Constants.h>

struct CustomConfigTags : StringEnum<MetaEnum::Tags, CustomConfigTags>
{
    static CustomConfigTags out_topic()
    {
        CustomConfigTags instance{"out_topic"};
        return instance;
    }
};

