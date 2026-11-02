#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <hyprland/src/plugins/PluginAPI.hpp>

inline HANDLE SCROLLOVERVIEW_HANDLE = nullptr;

bool ensureScrollOverviewHooks();
void disableScrollOverviewHooks();
bool consumeOverviewMouseAxisBind(uint32_t lastInputTimeMs);

void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles);
