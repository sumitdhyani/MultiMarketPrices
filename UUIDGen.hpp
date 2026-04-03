#pragma once
#include <random>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>

class UUIDGenerator
{
private:
    std::mt19937_64 rng;        // 64-bit Mersenne Twister

public:
    UUIDGenerator()
    {
        // Better seeding using random_device
        std::random_device rd;
        std::seed_seq ss{ rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd() };
        rng.seed(ss);
    }

    // 64-bit unsigned integer UUID (fast & simple)
    uint64_t generate64()
    {
        return rng();
    }

    // 128-bit UUID as two uint64_t (most common practical need)
    std::pair<uint64_t, uint64_t> generate128()
    {
        return { rng(), rng() };
    }

    // 128-bit UUID as string (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
    std::string generate128String()
    {
        auto [high, low] = generate128();

        std::stringstream ss;
        ss << std::hex << std::setfill('0')
           << std::setw(16) << high
           << std::setw(16) << low;

        std::string s = ss.str();
        // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        return s.substr(0,8) + "-" + s.substr(8,4) + "-" + s.substr(12,4) + 
               "-" + s.substr(16,4) + "-" + s.substr(20,12);
    }
};