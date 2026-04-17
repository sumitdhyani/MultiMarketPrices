#include <boost/asio/buffer.hpp>
#include <iostream>
#include <string>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <limits.h>
#include <cstring>
#include <functional>
#include <atomic>
#include <Constants.h>
#include <fstream>
#include <sstream>
#include <Logging.h>
#include "ConfigLib.h"

#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

namespace Config
{

namespace {

void watch_file(const std::string& dir,
                const std::string& filename,
                const std::function<void()>& listener,
                std::atomic<bool>& stopFlag)
{
    int fd = inotify_init();
    if (fd < 0) {
        perror("inotify_init");
        return;
    }

    int wd = inotify_add_watch(fd, dir.c_str(),
                               IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);

    if (wd == -1) {
        perror("inotify_add_watch");
        close(fd);
        return;
    }

    
    NANO_LOG(DEBUG, "Watching dir: %s", dir.c_str());
    char buffer[EVENT_BUF_LEN];

    while (!stopFlag) {
        int length = read(fd, buffer, EVENT_BUF_LEN);

        if (length < 0) {
            perror("read");
            break;
        }

        if (length == 0) {
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event =
                (struct inotify_event*)&buffer[i];

            if (event->len > 0) {
                if (std::strcmp(filename.c_str(), event->name) == 0) {
                    if (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {
                        std::cout << "Detected change in " << filename << std::endl;
                        listener();
                    }
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
}
}


std::optional<json::object> load_json(const std::string& path) {
    try
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file");
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return json::parse(buffer.str()).as_object();
    }
    catch(const std::exception& ex)
    {
        return std::nullopt;
    }

}

json::object merge(const json::object& app, const json::object& group, const json::object& system)
{
    json::object res;
    for(auto const&[k, v] : app)
    {
        res[k] = v;
    }

    for(auto const& [k,v] : group)
    {
        if (!res.contains(k)) res[k] = v;
    }

    for(auto const& [k,v] : system)
    {
        if (!res.contains(k)) res[k] = v;
    }

    return res;
}

json::object flatten(const json::object& config, const std::string& appId)
{
    json::object res;
    auto const& system_obj  = config.at(*ConfigTag::system()).as_object();
    auto const& groups_obj  = config.at(*ConfigTag::groups()).as_object();
    auto const& apps_obj          = config.at(*ConfigTag::apps()).as_object();

    auto const& app_obj = apps_obj.at(appId).as_object();
    auto const& app_group = app_obj.at(*ConfigTag::group()).as_string();
    auto const& group_obj = groups_obj.at(app_group).as_object();

    return merge(app_obj, group_obj, system_obj);
}

bool validate(const json::object& obj, const std::string& appId)
{
    if (!obj.contains(*ConfigTag::groups()))
    {
        NANO_LOG(DEBUG, "App section absent for app: %s", appId.c_str());
        return false;
    }

    auto const& groups = obj.at(*ConfigTag::groups()).as_object();
    if (!obj.contains(appId))
    {
        NANO_LOG(DEBUG, "App section absent for app: %s", appId.c_str());
        return false;
    }

    auto const& appSection = obj.at(appId).as_object();
    if (!appSection.contains(*ConfigTag::group()))
    {
        NANO_LOG(DEBUG, "Group identifier missing for app: %s", appId.c_str());
        return false;
    }

    const std::string group = appSection.at(*ConfigTag::group()).as_string().c_str();
    if (!groups.contains(group))
    {
        NANO_LOG(DEBUG, "Group identifier missing for app: %s", appId.c_str());
        return false;
    }

    return true;
}

std::optional<json::object> createConfig(const std::string& appId)
{
    auto config_opt = load_json("./config/config.json");
    if(!config_opt) return std::nullopt;

    auto& config = *config_opt;
    if (!validate(config, appId)) return std::nullopt;

    return flatten(config, appId);
}

void onConfigUpdate(const std::string& appId,
    const ConfigListener& configListener,
    const ConfigValidator& configValidator)
{
    NANO_LOG(DEBUG, "Config updated");
    auto cfg_opt = createConfig(appId);
    if(cfg_opt && configValidator(*cfg_opt))
    {
        std::cout << "New cfg: " << json::serialize(*cfg_opt) << std::endl;
        configListener(*cfg_opt);
    }
    else
    {
        std::cout << "Error with new cfg" << std::endl;
    }
}

std::optional<json::object> init(const std::string& appId,
    const ConfigListener& configListener,
    const ConfigValidator& configValidator)
{
    auto cfg_opt = createConfig(appId);
    if(cfg_opt)
    {
        std::cout << "New cfg: " << json::serialize(*cfg_opt) << std::endl;
    }
    else
    {
        std::cout << "Error with new cfg" << std::endl;
        return cfg_opt;
    }

    std::atomic<bool> stopFlag(false);
    std::thread([&stopFlag, appId, configListener, configValidator](){
        auto const listener = [appId, configListener, configValidator](){
            onConfigUpdate(appId, configListener, configValidator);
        };
        watch_file("./config", "config.json", listener, stopFlag);
    }).detach();
    return cfg_opt;

}


}