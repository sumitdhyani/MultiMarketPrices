#pragma once

#include <string>
#include <ctime>
#include <sys/stat.h>
#include <atomic>
#include <thread>
#include <chrono>

#include "NanoLogCpp17.h"

namespace Logging {

namespace detail {
    inline std::atomic<bool> rotationThreadRunning{false};
    inline std::string currentLogPath;
    inline std::string baseLogPath;
    inline std::atomic<int> currentLogNumber{0};
    inline constexpr size_t MAX_LOG_SIZE = 100ULL * 1024 * 1024; // 100 MB

    inline std::string generateLogFileName(const std::string& appId, int logNumber) {
        char dateBuf[16];
        std::time_t now = std::time(nullptr);
        std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", std::localtime(&now));
        return appId + "_" + dateBuf + "." + std::to_string(logNumber);
    }

    inline size_t getFileSize(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0)
            return static_cast<size_t>(st.st_size);
        return 0;
    }

    inline void rotationLoop() {
        while (rotationThreadRunning.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (getFileSize(currentLogPath) >= MAX_LOG_SIZE) {
                int nextNum = currentLogNumber.fetch_add(1, std::memory_order_relaxed) + 1;
                currentLogPath = generateLogFileName(baseLogPath, nextNum);
                NanoLog::setLogFile(currentLogPath.c_str());
            }
        }
    }
}

inline void init(const std::string& appId, const std::string& logDir = "./logs") {
    // Create log directory
    mkdir(logDir.c_str(), 0755);

    detail::baseLogPath = logDir + "/" + appId;
    detail::currentLogPath = detail::generateLogFileName(detail::baseLogPath, 0);
    detail::currentLogNumber.store(0, std::memory_order_relaxed);

    NanoLog::setLogFile(detail::currentLogPath.c_str());
    NanoLog::setLogLevel(NanoLog::LogLevels::DEBUG);
    NanoLog::preallocate();

    // Start background rotation monitor
    detail::rotationThreadRunning.store(true, std::memory_order_relaxed);
    std::thread(detail::rotationLoop).detach();
}

inline void shutdown() {
    detail::rotationThreadRunning.store(false, std::memory_order_relaxed);
    NanoLog::sync();
}

} // namespace Logging
