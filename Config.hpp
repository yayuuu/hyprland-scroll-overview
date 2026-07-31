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
};

enum class EScrollAction {
    WORKSPACE,
    COLUMN,
};

void registerDispatcher(const std::string& name, TDispatcher dispatcher);
void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword);
void registerConfig();

template <typename T>
CConfigValue<T>& valueRef(const std::string& name) {
    static std::unordered_map<std::string, UP<CConfigValue<T>>> values;

    const auto [it, inserted] = values.try_emplace(name);
    if (inserted)
        it->second = makeUnique<CConfigValue<T>>(name);

    return *it->second;
}

template <typename T>
T getValue(const std::string& name) {
    using TValue = std::decay_t<T>;

	if constexpr (std::is_same_v<TValue, ::Config::CGradientValueData>) {
        auto& ref = valueRef<::Config::IComplexConfigValue>(name);
        if (!ref.good())
            return {};
        return *sc<::Config::CGradientValueData*>(ref.ptr());
    }

    if constexpr (std::is_same_v<TValue, bool>)
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

int           getGestureDistance();
float         getScale();
bool          getAdaptiveScale();
float         getMinScale();
bool          getCardPlate();
float         getDropAnimationSpeed();
int           getWorkspaceGap();
ELayout       getLayout();
int           getScrollEventDelay();
bool          getLeftHanded();
int           getDragMode();
int           getDragThreshold();
float         getTouchpadScrollFactor();
float         getNavigateFactor();
bool          getNavigateInvert();
EScrollAction getVerticalScrollAction(ELayout layout);
EScrollAction getHorizontalScrollAction(ELayout layout);
int           getWallpaperMode();
bool          getBlur();
::Config::CCssGapData getCssGapData(const std::string& name);
int          getShadowEnabled();
int          getShadowRange();
int          getShadowRenderPower();
std::optional<::Config::CGradientValueData> getShadowColor();

}
