#include <memory>
#include <string>
#include <atomic>
#include <optional>
#include <sys/inotify.h>
#include <unistd.h>
#include <limits.h>
#include <cstring>
#include <functional>
#include <boost/json.hpp>
namespace json = boost::json;

namespace Config
{
    namespace
    {
        void watch_file(const std::string& dir,
                const std::string& filename,
                const std::function<void()>& listener,
                std::atomic<bool>& stopFlag);
    }

    using ConfigListener = std::function<void(const json::object& config)>;
    using ConfigValidator = std::function<bool(const json::object& config)>;

    std::optional<json::object> init(const std::string& appId,
        const ConfigListener& configListener,
        const ConfigValidator& configValidator);

    void stop();
}
