#include "Config.hpp"

#include <algorithm>
#include <config/shared/complex/ComplexDataType.hpp>
#include <config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/GradientValue.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <regex>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

ScrollOverview::Config::TGestureRegistrar g_gestureRegistrar   = nullptr;

int dispatcherFactoryLua(lua_State* L, std::string_view name);

using TArgValidator = bool (*)(std::string_view);

struct SDispatcher {
    std::string_view                    name;
    std::regex                          argPattern;
    std::string_view                    defaultArg;
    std::string_view                    typeArgError;
    std::string_view                    invalidArgError;
    TArgValidator                       argValidator = nullptr;
    ScrollOverview::Config::TDispatcher dispatcher   = nullptr;
    lua_CFunction                       luaFunction  = nullptr;

    bool isArgValid(const std::string_view arg) const {
        if (argValidator)
            return argValidator(arg);

        return std::regex_match(arg.begin(), arg.end(), argPattern);
    }
};

SDispatcher* findDispatcher(const std::string_view name) {
    static SDispatcher registrations[] = {
        {
            .name            = "overview",
            .argPattern      = std::regex{R"(^(select|(toggle|on|open|enable|off|close|disable)([ \t]+(all|[^ \t"\\]+))?)$)"},
            .defaultArg      = "toggle",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected select or toggle/open/close [monitor|all] (aliases: on, enable, off, disable)",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview"); },
        },
        {
            .name            = "navigate",
            .argPattern      = std::regex{"^(left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: left, right, up, down",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "navigate"); },
        },
        {
            .name            = "window",
            .argPattern      = std::regex{"^(select|close)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: select, close",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "window"); },
        },
    };

    const auto MATCH = std::ranges::find_if(registrations, [name](const auto& registration) { return registration.name == name; });
    return MATCH == std::end(registrations) ? nullptr : &*MATCH;
}

int runDispatcherNow(lua_State* L, const SDispatcher& dispatcher, const char* arg) {
    if (!dispatcher.dispatcher)
        return luaL_error(L, "%s: dispatcher is not registered", dispatcher.name.data());

    const auto result = dispatcher.dispatcher(arg);
    if (!result.success)
        return luaL_error(L, "%s: %s", dispatcher.name.data(), result.error.c_str());

    return 0;
}

void pushDispatcherBindAction(lua_State* L, const char* name, const char* arg) {
    const std::string CODE = "return function() return hl.plugin.scrolloverview._dispatch(\"" + std::string{name} + "\", \"" + std::string{arg} + "\") end";

    if (luaL_loadstring(L, CODE.c_str()) != LUA_OK)
        lua_error(L);

    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        lua_error(L);
}

int runDispatcherActionLua(lua_State* L) {
    if (lua_gettop(L) < 2 || lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2))
        return luaL_error(L, "_dispatch: expected dispatcher name and argument");

    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "_dispatch: expected string arguments");

    const char* name = lua_tostring(L, 1);
    const char* arg  = lua_tostring(L, 2);
    const auto  DISPATCHER = findDispatcher(name);

    if (!DISPATCHER)
        return luaL_error(L, "_dispatch: unknown dispatcher '%s'", name);
    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", name, arg, DISPATCHER->invalidArgError.data());

    return runDispatcherNow(L, *DISPATCHER, arg);
}

int dispatcherFactoryLua(lua_State* L, std::string_view name) {
    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return luaL_error(L, "%s: dispatcher metadata is not registered", name.data());

    const char* arg = DISPATCHER->defaultArg.empty() ? nullptr : DISPATCHER->defaultArg.data();

    if (!arg && (lua_gettop(L) < 1 || lua_isnoneornil(L, 1)))
        return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

    if (lua_gettop(L) >= 1) {
        if (lua_isnoneornil(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        if (!lua_isstring(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        arg = lua_tostring(L, 1);
    }

    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", DISPATCHER->name.data(), arg, DISPATCHER->invalidArgError.data());

    if (g_pKeybindManager && g_pKeybindManager->m_currentKeybind && g_pKeybindManager->m_currentKeybind->handler == "__lua") {
        return runDispatcherNow(L, *DISPATCHER, arg);
    }

    pushDispatcherBindAction(L, DISPATCHER->name.data(), arg);

    return 1;
}

int configureLua(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "configure: expected a table");

    const int CONFIG = lua_absindex(L, 1);

    lua_getglobal(L, "hl");
    if (!lua_istable(L, -1))
        return luaL_error(L, "configure: global hl table is not available");

    lua_getfield(L, -1, "config");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "configure: hl.config is not available");
    }

    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, CONFIG);
    lua_setfield(L, -2, "scrolloverview");
    lua_setfield(L, -2, "plugin");

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        lua_pop(L, 2);
        return luaL_error(L, "configure: %s", err ? err : "hl.config failed");
    }

    lua_pop(L, 1);

    return 0;
}

int gestureLua(lua_State* L) {
    if (!g_gestureRegistrar)
        return luaL_error(L, "gesture: registrar is not registered");

    if (!lua_istable(L, 1))
        return luaL_error(L, "gesture: expected a table, e.g. { fingers = 3, direction = \"up\" }");

    lua_getfield(L, 1, "fingers");
    if (!lua_isinteger(L, -1))
        return luaL_error(L, "gesture: 'fingers' (integer) is required");
    const size_t FINGERS = sc<size_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "direction");
    if (!lua_isstring(L, -1))
        return luaL_error(L, "gesture: 'direction' (string) is required");
    const std::string DIRECTION = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "action");
    const std::string ACTION = lua_isstring(L, -1) ? lua_tostring(L, -1) : "overview";
    lua_pop(L, 1);

    // accept either "mods" or "mod"
    lua_getfield(L, 1, "mods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "mod");
    }
    const std::string MODS = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    lua_getfield(L, 1, "scale");
    const float SCALE = lua_isnumber(L, -1) ? sc<float>(lua_tonumber(L, -1)) : 1.F;
    lua_pop(L, 1);

    lua_getfield(L, 1, "disable_inhibit");
    const bool DISABLE_INHIBIT = lua_toboolean(L, -1);
    lua_pop(L, 1);

    const auto result = g_gestureRegistrar(FINGERS, DIRECTION, ACTION, MODS, SCALE, DISABLE_INHIBIT);
    if (!result.success)
        return luaL_error(L, "gesture: %s", result.error.c_str());

    return 0;
}

}

namespace ScrollOverview::Config {

void registerDispatcher(const std::string& name, TDispatcher dispatcher) {
    HyprlandAPI::addDispatcherV2(SCROLLOVERVIEW_HANDLE, "scrolloverview:" + name, dispatcher);

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return;

    DISPATCHER->dispatcher = dispatcher;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", std::string{DISPATCHER->name}, DISPATCHER->luaFunction);
}

void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword) {
    HyprlandAPI::addConfigKeyword(SCROLLOVERVIEW_HANDLE, "scrolloverview-gesture", gestureKeyword, {});

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    g_gestureRegistrar = gestureRegistrar;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "gesture", ::gestureLua);
}

static void registerLuaFunctions() {
    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "_dispatch", ::runDispatcherActionLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "configure", ::configureLua);
}

static void registerConfigValues() {
    using namespace ::Config::Values;

    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:gesture_distance", "gesture distance in pixels", 200, SIntValueOptions{.min = 1}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:scrolloverview:scale", "overview scale", 0.5F, SFloatValueOptions{.min = 0.1F, .max = 0.9F}));
    // Off by default: fit-all only genuinely works up to ~4 workspaces on a 2880px screen (past that
    // the min_scale floor takes over and the carousel scrolls anyway), and it was paid for with
    // cards resizing whenever a workspace appeared or disappeared, smaller drop targets and
    // unreadable previews. niri takes the same position with its fixed `zoom`. The pill bar already
    // draws a dot per workspace over the overview, so "what exists" is answered without shrinking.
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:scrolloverview:adaptive_scale", "shrink cards so every workspace fits on screen", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:scrolloverview:min_scale", "floor for adaptive_scale; past it the carousel scrolls instead", 0.18F,
                                                          SFloatValueOptions{.min = 0.05F, .max = 0.9F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:scrolloverview:card_plate", "draw a plate behind every card so empty workspaces stay visible", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:scrolloverview:drop_animation_speed",
                                                          "duration of the drop transition, in units of 100ms like Hyprland animation speeds", 2.5F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 30.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:workspace_gap", "gap between overview workspaces", 0, SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:scrolloverview:layout", "overview layout", Hyprlang::STRING{"vertical"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:input:scroll_event_delay", "minimum delay (ms) between discrete scroll steps (wheel workspace nav and trackpad focus stepping)", 200, SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:scrolloverview:input:touchpad_scroll_factor", "overview touchpad scroll factor", 1.F,
                                                          SFloatValueOptions{.min = 0.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:scrolloverview:input:navigate_factor",
                                                          "carousel travel per px of finger travel for the navigate trackpad gesture", 1.F, SFloatValueOptions{.min = 0.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:scrolloverview:input:navigate_invert",
                                                         "invert the navigate trackpad gesture: cards move against the fingers instead of with them", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:input:left_handed", "overview left handed mouse buttons, 2 follows input:left_handed", 2,
                                                        SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:input:scrolling_mode", "overview mouse wheel behavior", 0,
                                                        SIntValueOptions{.min = 0, .max = 3}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:input:drag_mode", "overview mouse drag behavior", 0,
                                                        SIntValueOptions{.min = 0, .max = 1}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:input:drag_threshold", "overview drag threshold", 10,
                                                        SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:wallpaper", "wallpaper mode", 0, SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, makeShared<CBoolValue>("plugin:scrolloverview:blur", "blur the overview wallpaper", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:scrolloverview:shadow:enabled", "draw a shadow around each workspace card", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:shadow:range", "workspace card shadow range", -1));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:scrolloverview:shadow:render_power", "workspace card shadow render power", -1));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CGradientValue>("plugin:scrolloverview:shadow:color", "workspace card shadow color", -1));
}

void registerConfig() {
    registerLuaFunctions();
    registerConfigValues();
    HyprlandAPI::reloadConfig();
}

int getGestureDistance() {
    return std::max<int>(1, getValue<int>("plugin:scrolloverview:gesture_distance"));
}

bool getAdaptiveScale() {
    return getValue<bool>("plugin:scrolloverview:adaptive_scale");
}

float getMinScale() {
    return std::clamp(getValue<float>("plugin:scrolloverview:min_scale"), 0.05F, 0.9F);
}

float getDropAnimationSpeed() {
    return std::clamp(getValue<float>("plugin:scrolloverview:drop_animation_speed"), 0.1F, 30.F);
}

bool getCardPlate() {
    return getValue<bool>("plugin:scrolloverview:card_plate");
}

float getScale() {
    return std::clamp(getValue<float>("plugin:scrolloverview:scale"), 0.1F, 0.9F);
}

int getWorkspaceGap() {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:workspace_gap"));
}

ELayout getLayout() {
    const auto LAYOUT = getValue<std::string>("plugin:scrolloverview:layout");
    return LAYOUT == "horizontal" ? ELayout::HORIZONTAL : ELayout::VERTICAL;
}

bool getLeftHanded() {
    const auto LEFT_HANDED = getValue<int>("plugin:scrolloverview:input:left_handed");
    if (LEFT_HANDED <= 1)
        return LEFT_HANDED != 0;

    return getValue<bool>("input:left_handed");
}

int getDragMode() {
    return std::clamp(getValue<int>("plugin:scrolloverview:input:drag_mode"), 0, 1);
}

int getDragThreshold() {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:input:drag_threshold"));
}

float getTouchpadScrollFactor() {
    static constexpr float OVERVIEWTOUCHPADSCROLLFACTOR = 1.5F;

    return OVERVIEWTOUCHPADSCROLLFACTOR * std::max<float>(0.F, getValue<float>("input:touchpad:scroll_factor")) *
        std::max<float>(0.F, getValue<float>("plugin:scrolloverview:input:touchpad_scroll_factor"));
}

float getNavigateFactor() {
    return std::max<float>(0.F, getValue<float>("plugin:scrolloverview:input:navigate_factor"));
}

bool getNavigateInvert() {
    return getValue<bool>("plugin:scrolloverview:input:navigate_invert");
}

static EScrollAction defaultVerticalScrollAction(ELayout layout) {
    return layout == ELayout::HORIZONTAL ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

EScrollAction getVerticalScrollAction(ELayout layout) {
    const auto MODE = std::clamp(getValue<int>("plugin:scrolloverview:input:scrolling_mode"), 0, 3);

    switch (MODE) {
        case 1: return defaultVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
        case 2: return EScrollAction::WORKSPACE;
        case 3: return EScrollAction::COLUMN;
        case 0:
        default: return defaultVerticalScrollAction(layout);
    }
}

EScrollAction getHorizontalScrollAction(ELayout layout) {
    return getVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

int getScrollEventDelay() {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:input:scroll_event_delay"));
}

int getWallpaperMode() {
    return std::clamp<int>(getValue<int>("plugin:scrolloverview:wallpaper"), 0, 2);
}

bool getBlur() {
    return getValue<bool>("plugin:scrolloverview:blur");
}

::Config::CCssGapData getCssGapData(const std::string& name) {
    auto& VALUE = valueRef<::Config::IComplexConfigValue>(name);
    if (!VALUE.good())
        return {};

    auto* const GAPS = dc<::Config::CCssGapData*>(VALUE.ptr());
    if (!GAPS)
        return {};

    return *GAPS;
}

int getShadowEnabled() {
    return getValue<bool>("plugin:scrolloverview:shadow:enabled") ? 1 : 0;
}

int getShadowRange() {
    return getValue<int>("plugin:scrolloverview:shadow:range");
}

int getShadowRenderPower() {
    return getValue<int>("plugin:scrolloverview:shadow:render_power");
}

std::optional<::Config::CGradientValueData> getShadowColor() {
    constexpr auto NAME = "plugin:scrolloverview:shadow:color";

    if (!::Config::mgr()->getConfigValue(NAME).setByUser)
        return std::nullopt;

    return getValue<::Config::CGradientValueData>(NAME);
}

}
