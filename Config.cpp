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
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>

#include <hyprland/src/config/lua/types/LuaConfigUtils.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>

#include <regex>
#include <cmath>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

ScrollOverview::Config::TGestureRegistrar g_gestureRegistrar   = nullptr;

using TMonitorValues = std::unordered_map<std::string, UP<::Config::Lua::ILuaConfigValue>>;
std::unordered_map<std::string, TMonitorValues> g_monitorValues;
std::unordered_map<std::string, SP<::Config::Values::IValue>> g_configDefinitions;
CHyprSignalListener g_configPreReloadHook;

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

    const auto& ACTIONSTATE = Config::Actions::state();
    if (ACTIONSTATE && ACTIONSTATE->m_bindInvocationDepth > 0) {
        return runDispatcherNow(L, *DISPATCHER, arg);
    }

    pushDispatcherBindAction(L, DISPATCHER->name.data(), arg);

    return 1;
}

std::optional<std::string> validateMonitorRange(const SP<::Config::Values::IValue>& definition, ::Config::Lua::ILuaConfigValue& value) {
    const auto check = [](const auto& definition, auto data) -> std::optional<std::string> {
        if (definition.m_min && data < *definition.m_min)
            return std::format("value {} is less than the minimum of {}", data, *definition.m_min);
        if (definition.m_max && data > *definition.m_max)
            return std::format("value {} is more than the maximum of {}", data, *definition.m_max);
        return std::nullopt;
    };
    // Some Hyprland versions do not preserve bounds in fromGenericValue().
    if (const auto integer = dynamic_cast<::Config::Values::CIntValue*>(definition.get()))
        return check(*integer, value.asInt());
    if (const auto number = dynamic_cast<::Config::Values::CFloatValue*>(definition.get())) {
        if (!std::isfinite(value.asFloat()))
            return "expected a finite number";
        return check(*number, value.asFloat());
    }
    return std::nullopt;
}

std::optional<std::string> configureMonitor(lua_State* L, const std::string& output) {
    TMonitorValues pending;
    const auto parseTable = [&](auto&& self, int table, const std::string& prefix) -> std::optional<std::string> {
        table = lua_absindex(L, table);
        lua_pushnil(L);
        while (lua_next(L, table)) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 2);
                return "option names must be strings";
            }

            const std::string key = lua_tostring(L, -2);
            if (prefix.empty() && key == "output") {
                lua_pop(L, 1);
                continue;
            }

            const auto path = prefix.empty() ? key : prefix + ":" + key;
            const auto name = "plugin:scrolloverview:" + path;
            if (const auto definition = g_configDefinitions.find(name); definition != g_configDefinitions.end()) {
                auto value = ::Config::Lua::fromGenericValue(definition->second);
                if (!value) {
                    lua_pop(L, 2);
                    return "unsupported option '" + path + "'";
                }

                const auto error = value->parse(L);
                if (error.errorCode != ::Config::Lua::PARSE_ERROR_OK) {
                    lua_pop(L, 2);
                    return path + ": " + error.message;
                }
                if (const auto error = validateMonitorRange(definition->second, *value)) {
                    lua_pop(L, 2);
                    return path + ": " + *error;
                }
                pending.insert_or_assign(name, std::move(value));
            } else {
                const bool category = std::ranges::any_of(g_configDefinitions, [&name](const auto& entry) { return entry.first.starts_with(name + ":"); });
                if (!category || !lua_istable(L, -1)) {
                    lua_pop(L, 2);
                    return category ? path + ": expected a table" : "unknown option '" + path + "'";
                }
                if (const auto error = self(self, -1, path)) {
                    lua_pop(L, 2);
                    return error;
                }
            }
            lua_pop(L, 1);
        }
        return std::nullopt;
    };

    if (const auto error = parseTable(parseTable, 1, ""))
        return error;

    // Commit only after every field has passed the same parser as global config.
    // Keep sparse overrides so later global changes remain visible to this output.
    auto& values = g_monitorValues[output];
    for (auto& [name, value] : pending)
        values.insert_or_assign(name, std::move(value));
    return std::nullopt;
}

int configureLua(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "configure: expected a table");

    lua_getfield(L, 1, "output");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING || lua_rawlen(L, -1) == 0)
            return luaL_error(L, "configure: output must be a non-empty monitor name");

        // Leave C++ scopes before lua_error performs its longjmp.
        bool failed = false;
        {
            const std::string output = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (const auto error = configureMonitor(L, output)) {
                lua_pushfstring(L, "configure: output %s: %s", output.c_str(), error->c_str());
                failed = true;
            }
        }
        return failed ? lua_error(L) : 0;
    }
    lua_pop(L, 1);

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

const void* monitorValueData(const std::string& name, PHLMONITOR monitor) {
    if (!monitor)
        return nullptr;

    const auto output = g_monitorValues.find(monitor->m_name);
    if (output == g_monitorValues.end())
        return nullptr;

    const auto value = output->second.find(name);
    return value == output->second.end() ? nullptr : value->second->data();
}

bool hasCrossMonitorDragEnabled() {
    constexpr auto NAME = "plugin:scrolloverview:cross_monitor_drag";
    if (getValue<bool>(NAME))
        return true;

    return std::ranges::any_of(g_monitorValues, [NAME](const auto& output) {
        const auto value = output.second.find(NAME);
        return value != output.second.end() && value->second->asInt() != 0;
    });
}

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

static void defineConfigValues() {
    using namespace ::Config::Values;
    const auto addValue = [](SP<IValue> value) {
        g_configDefinitions.emplace(value->name(), value);
    };

    addValue(makeShared<CIntValue>("plugin:scrolloverview:gesture_distance", "gesture distance in pixels", 200, SIntValueOptions{.min = 1}));
    addValue(makeShared<CFloatValue>("plugin:scrolloverview:scale", "overview scale", 0.5F, SFloatValueOptions{.min = 0.1F, .max = 0.9F}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:workspace_gap", "gap between overview workspaces", 0, SIntValueOptions{.min = 0}));
    addValue(makeShared<CStringValue>("plugin:scrolloverview:layout", "overview layout", Hyprlang::STRING{"vertical"}));
    addValue(makeShared<CBoolValue>("plugin:scrolloverview:cross_monitor_drag", "enable cross-monitor window dragging", false));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:input:scroll_event_delay", "minimum delay (ms) between discrete scroll steps (wheel workspace nav and trackpad focus stepping)", 200, SIntValueOptions{.min = 0}));
    addValue(makeShared<CFloatValue>("plugin:scrolloverview:input:touchpad_scroll_factor", "overview touchpad scroll factor", 1.F,
                                     SFloatValueOptions{.min = 0.F}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:input:left_handed", "overview left handed mouse buttons, 2 follows input:left_handed", 2,
                                   SIntValueOptions{.min = 0, .max = 2}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:input:scrolling_mode", "overview mouse wheel behavior", 0,
                                   SIntValueOptions{.min = 0, .max = 3}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:input:drag_mode", "overview mouse drag behavior", 0,
                                   SIntValueOptions{.min = 0, .max = 1}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:input:drag_threshold", "overview drag threshold", 10,
                                   SIntValueOptions{.min = 0}));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:wallpaper", "wallpaper mode", 0, SIntValueOptions{.min = 0, .max = 2}));
    addValue(makeShared<CBoolValue>("plugin:scrolloverview:blur", "blur the overview wallpaper", false));
    addValue(makeShared<CBoolValue>("plugin:scrolloverview:shadow:enabled", "draw a shadow around each workspace card", false));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:shadow:range", "workspace card shadow range", -1));
    addValue(makeShared<CIntValue>("plugin:scrolloverview:shadow:render_power", "workspace card shadow render power", -1));
    addValue(makeShared<CGradientValue>("plugin:scrolloverview:shadow:color", "workspace card shadow color", -1));
}

static void initializeMonitorConfig() {
    g_monitorValues.clear();
    g_configDefinitions.clear();
    g_configPreReloadHook = Event::bus()->m_events.config.preReload.listen([] { g_monitorValues.clear(); });
    defineConfigValues();
}

void registerConfig() {
    initializeMonitorConfig();
    registerLuaFunctions();
    for (const auto& [name, value] : g_configDefinitions)
        HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, value);
    HyprlandAPI::reloadConfig();
}

int getGestureDistance(PHLMONITOR monitor) {
    return std::max<int>(1, getValue<int>("plugin:scrolloverview:gesture_distance", monitor));
}

float getScale(PHLMONITOR monitor) {
    return std::clamp(getValue<float>("plugin:scrolloverview:scale", monitor), 0.1F, 0.9F);
}

int getWorkspaceGap(PHLMONITOR monitor) {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:workspace_gap", monitor));
}

ELayout getLayout(PHLMONITOR monitor) {
    const auto LAYOUT = getValue<std::string>("plugin:scrolloverview:layout", monitor);
    if (LAYOUT == "auto")
        return ELayout::AUTO;

    return LAYOUT == "horizontal" ? ELayout::HORIZONTAL : ELayout::VERTICAL;
}

bool getCrossMonitorDrag(PHLMONITOR monitor) {
    return getValue<bool>("plugin:scrolloverview:cross_monitor_drag", monitor);
}

bool getLeftHanded(PHLMONITOR monitor) {
    const auto LEFT_HANDED = getValue<int>("plugin:scrolloverview:input:left_handed", monitor);
    if (LEFT_HANDED <= 1)
        return LEFT_HANDED != 0;

    return getValue<bool>("input:left_handed");
}

int getDragMode(PHLMONITOR monitor) {
    return std::clamp(getValue<int>("plugin:scrolloverview:input:drag_mode", monitor), 0, 1);
}

int getDragThreshold(PHLMONITOR monitor) {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:input:drag_threshold", monitor));
}

float getTouchpadScrollFactor(PHLMONITOR monitor) {
    static constexpr float OVERVIEWTOUCHPADSCROLLFACTOR = 1.5F;

    return OVERVIEWTOUCHPADSCROLLFACTOR * std::max<float>(0.F, getValue<float>("input:touchpad:scroll_factor")) *
        std::max<float>(0.F, getValue<float>("plugin:scrolloverview:input:touchpad_scroll_factor", monitor));
}

static EScrollAction defaultVerticalScrollAction(ELayout layout) {
    return layout == ELayout::HORIZONTAL ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

EScrollAction getVerticalScrollAction(ELayout layout, PHLMONITOR monitor) {
    const auto MODE = std::clamp(getValue<int>("plugin:scrolloverview:input:scrolling_mode", monitor), 0, 3);

    switch (MODE) {
        case 1: return defaultVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
        case 2: return EScrollAction::WORKSPACE;
        case 3: return EScrollAction::COLUMN;
        case 0:
        default: return defaultVerticalScrollAction(layout);
    }
}

EScrollAction getHorizontalScrollAction(ELayout layout, PHLMONITOR monitor) {
    return getVerticalScrollAction(layout, monitor) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

int getScrollEventDelay(PHLMONITOR monitor) {
    return std::max<int>(0, getValue<int>("plugin:scrolloverview:input:scroll_event_delay", monitor));
}

int getWallpaperMode(PHLMONITOR monitor) {
    return std::clamp<int>(getValue<int>("plugin:scrolloverview:wallpaper", monitor), 0, 2);
}

bool getBlur(PHLMONITOR monitor) {
    return getValue<bool>("plugin:scrolloverview:blur", monitor);
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

int getShadowEnabled(PHLMONITOR monitor) {
    return getValue<bool>("plugin:scrolloverview:shadow:enabled", monitor) ? 1 : 0;
}

int getShadowRange(PHLMONITOR monitor) {
    return getValue<int>("plugin:scrolloverview:shadow:range", monitor);
}

int getShadowRenderPower(PHLMONITOR monitor) {
    return getValue<int>("plugin:scrolloverview:shadow:render_power", monitor);
}

std::optional<::Config::CGradientValueData> getShadowColor(PHLMONITOR monitor) {
    constexpr auto NAME = "plugin:scrolloverview:shadow:color";

    if (!monitorValueData(NAME, monitor) && !::Config::mgr()->getConfigValue(NAME).setByUser)
        return std::nullopt;

    return getValue<::Config::CGradientValueData>(NAME, monitor);
}

}
