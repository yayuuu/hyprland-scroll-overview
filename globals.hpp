#pragma once

#include <cstdint>
#include <hyprland/src/plugins/PluginAPI.hpp>

inline HANDLE SCROLLOVERVIEW_HANDLE = nullptr;

bool ensureScrollOverviewHooks();
void disableScrollOverviewHooks();
bool consumeOverviewMouseAxisBind(uint32_t lastInputTimeMs);
