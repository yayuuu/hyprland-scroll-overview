#pragma once

#include "globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace ScrollOverview::Config {

using TDispatcher       = SDispatchResult (*)(std::string);
using TGestureKeyword   = Hyprlang::CParseResult (*)(const char* LHS, const char* RHS);
using TGestureRegistrar   = SDispatchResult (*)(size_t fingerCount, const std::string& direction, const std::string& action, const std::string& mods, float deltaScale,
                                                bool disableInhibit);

enum class ELayout {
    VERTICAL,
    HORIZONTAL,
    AUTO,
};

enum class EScrollAction {
    WORKSPACE,
    COLUMN,
};

enum class EWallpaperLayer {
    AUTO,
    BACKGROUND,
    BOTTOM,
};

void registerDispatcher(const std::string& name, TDispatcher dispatcher);
void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword);
void registerConfig();
const void* monitorValueData(const std::string& name, PHLMONITOR monitor);
bool hasCrossMonitorDragEnabled();

template <typename T>
CConfigValue<T>& valueRef(const std::string& name) {
    static std::unordered_map<std::string, UP<CConfigValue<T>>> values;

    const auto [it, inserted] = values.try_emplace(name);
    if (inserted)
        it->second = makeUnique<CConfigValue<T>>(name);

    return *it->second;
}

template <typename T>
T getValue(const std::string& name, PHLMONITOR monitor = {}) {
    using TValue = std::decay_t<T>;

    if (const auto DATA = monitorValueData(name, monitor)) {
        if constexpr (std::is_same_v<TValue, ::Config::CGradientValueData>)
            return *static_cast<const ::Config::CGradientValueData*>(static_cast<const ::Config::IComplexConfigValue*>(DATA));
        else if constexpr (std::is_same_v<TValue, bool>)
            return *static_cast<const bool*>(DATA);
        else if constexpr (std::is_integral_v<TValue>)
            return static_cast<TValue>(*static_cast<const Hyprlang::INT*>(DATA));
        else if constexpr (std::is_floating_point_v<TValue>)
            return static_cast<TValue>(*static_cast<const Hyprlang::FLOAT*>(DATA));
        else
            return *static_cast<const TValue*>(DATA);
    }

    if constexpr (std::is_same_v<TValue, ::Config::CGradientValueData>) {
        auto& ref = valueRef<::Config::IComplexConfigValue>(name);
        if (!ref.good())
            return {};
        return *sc<::Config::CGradientValueData*>(ref.ptr());
    } else if constexpr (std::is_same_v<TValue, bool>)
        return *valueRef<Hyprlang::INT>(name) != 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        return sc<TValue>(*valueRef<Hyprlang::INT>(name));
    else if constexpr (std::is_floating_point_v<TValue>)
        return sc<TValue>(*valueRef<Hyprlang::FLOAT>(name));
    else
        return *valueRef<TValue>(name);
}

template <typename T>
T* getValuePtr(const std::string& name) {
    return valueRef<T>(name).ptr();
}

template <typename T>
void setValue(const std::string& name, const T& value) {
    using TValue = std::decay_t<T>;

    if constexpr (std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = value ? 1 : 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = sc<Hyprlang::INT>(value);
    else if constexpr (std::is_floating_point_v<TValue>)
        *getValuePtr<Hyprlang::FLOAT>(name) = sc<Hyprlang::FLOAT>(value);
    else
        *getValuePtr<TValue>(name) = value;
}

int           getGestureDistance(PHLMONITOR monitor);
float         getScale(PHLMONITOR monitor);
int           getWorkspaceGap(PHLMONITOR monitor);
ELayout       getLayout(PHLMONITOR monitor);
bool          getCrossMonitorDrag(PHLMONITOR monitor);
int           getScrollEventDelay(PHLMONITOR monitor);
bool          getLeftHanded(PHLMONITOR monitor);
int           getDragMode(PHLMONITOR monitor);
int           getDragThreshold(PHLMONITOR monitor);
float         getTouchpadScrollFactor(PHLMONITOR monitor);
EScrollAction getVerticalScrollAction(ELayout layout, PHLMONITOR monitor);
EScrollAction getHorizontalScrollAction(ELayout layout, PHLMONITOR monitor);
int           getWallpaperMode(PHLMONITOR monitor);
EWallpaperLayer getWallpaperLayer(PHLMONITOR monitor);
bool          getBlur(PHLMONITOR monitor);
::Config::CCssGapData getCssGapData(const std::string& name);
int          getShadowEnabled(PHLMONITOR monitor);
int          getShadowRange(PHLMONITOR monitor);
int          getShadowRenderPower(PHLMONITOR monitor);
std::optional<::Config::CGradientValueData> getShadowColor(PHLMONITOR monitor);

}
