#pragma once
// Compiled only in the disposable-compositor test build.
#if defined(SCROLLOVERVIEW_TESTING)
#include "OverviewGesture.hpp"
#include "IOverview.hpp"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
inline std::unique_ptr<COverviewGesture> testOverviewGesture;
inline int testOverviewGestureLua(lua_State* L) {
    const std::string op = luaL_checkstring(L, 1);
    static eTrackpadGestureDirection direction = TRACKPAD_GESTURE_DIR_UP;
    IPointer::SSwipeUpdateEvent update{.fingers = 4};
    IPointer::SSwipeEndEvent end{.cancelled = lua_toboolean(L, 3) != 0};
    if (op == "begin_up" || op == "begin_down") {
        const bool down = op == "begin_down";
        direction = down ? TRACKPAD_GESTURE_DIR_DOWN : TRACKPAD_GESTURE_DIR_UP;
        testOverviewGesture = std::make_unique<COverviewGesture>(down, true);
        testOverviewGesture->begin({.swipe = &update, .direction = direction, .scale = 1});
    } else if (op == "update" && testOverviewGesture) {
        update.delta.y = luaL_checknumber(L, 2) * (direction == TRACKPAD_GESTURE_DIR_UP ? -1 : 1);
        testOverviewGesture->update({.swipe = &update, .direction = direction, .scale = 1});
    } else if (op == "end" && testOverviewGesture) {
        testOverviewGesture->end({.swipe = &end, .direction = direction, .scale = 1});
        testOverviewGesture.reset();
    }
    lua_pushinteger(L, scrollOverviews().size());
    lua_pushboolean(L, !scrollOverviews().empty() && scrollOverviews().front()->m_isSwiping);
    const auto overview = dynamic_cast<CScrollOverview*>(activeScrollOverview().get());
    const auto status = overview ? overview->testStatus() : "no overview";
    lua_pushstring(L, status.c_str());
    return 3;
}
inline void registerOverviewGestureProbe() {
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "_test_gesture", testOverviewGestureLua);
}
#else
inline void registerOverviewGestureProbe() {}
#endif
