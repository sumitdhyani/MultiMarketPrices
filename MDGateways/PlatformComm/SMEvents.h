#pragma once
#include <string>
#include <TypeWrapper.h>
#include <boost/json.hpp>

using Dictionary = boost::json::object;

// Dictionary must have the instrument key and list of destination topics
struct SubscriptionRecord : TypeWrapper<Dictionary> {};

// Dictionary must have the laset consumed offset 
struct DownloadEnd : TypeWrapper<Dictionary> {};

struct Instrument : TypeWrapper<std::string> {};

struct SubscriptionType : TypeWrapper<std::string> {};

struct Revoke{};

struct Assign{};

struct Partition : TypeWrapper<int32_t>{};
