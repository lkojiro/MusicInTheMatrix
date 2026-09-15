#include "mitm/global_config.hpp"

#include <mutex>

namespace mitm {

namespace {
std::mutex g_mutex;
GlobalConfig g_config;
} // namespace

GlobalConfig getGlobalConfig() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config;
}

void setGlobalConfig(const GlobalConfig& config) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = config;
}

void updateGlobalConfig(const std::function<void(GlobalConfig&)>& mutator) {
    std::lock_guard<std::mutex> lock(g_mutex);
    mutator(g_config);
}

} // namespace mitm
