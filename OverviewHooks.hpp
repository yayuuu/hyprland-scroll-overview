#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>

#if defined(__aarch64__)
#include "Arm64Hook.hpp"
#include <memory>
#include <vector>
using OverviewFunctionHook = Arm64Hook;
inline std::vector<std::unique_ptr<Arm64Hook>> overviewArmHooks;

inline OverviewFunctionHook* createOverviewFunctionHook(HANDLE, const void* source, const void* destination) {
    auto hook = std::make_unique<Arm64Hook>(const_cast<void*>(source), const_cast<void*>(destination));
    auto* result = hook.get();
    overviewArmHooks.push_back(std::move(hook));
    return result;
}
inline void clearOverviewFunctionHooks() {
    for (auto it = overviewArmHooks.rbegin(); it != overviewArmHooks.rend(); ++it) {
        if (!(*it)->unhook())
            throw std::runtime_error("Failed restoring ARM64 overview hook");
    }
    overviewArmHooks.clear();
}
#else
using OverviewFunctionHook = CFunctionHook;
inline OverviewFunctionHook* createOverviewFunctionHook(HANDLE owner, const void* source, const void* destination) {
    return HyprlandAPI::createFunctionHook(owner, source, destination);
}
inline void clearOverviewFunctionHooks() {}
#endif
