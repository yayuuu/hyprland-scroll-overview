#include <any>
#include <chrono>
#include <sstream>

#define private public
#include <hyprland/src/layout/supplementary/DragController.hpp>
#undef private

#include "NativeDrag.hpp"

void finishNativeDragAdoption(Layout::Supplementary::CDragStateController* dragController) {
    if (dragController)
        dragController->m_dragMode = MBIND_INVALID;
}
