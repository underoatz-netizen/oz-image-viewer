// oz/viewer/context_menu.hpp - testable pure context-menu plan seam (Stage 9.2).
#pragma once

#include "ui_command_state.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace oz::viewer {

enum class context_menu_item_type {
    command,
    separator
};

struct context_menu_item {
    context_menu_item_type type = context_menu_item_type::command;
    int command_id = 0;          // e.g. 302, 301, 105
    std::wstring text;           // e.g. L"Fit to Window\tF"
    bool enabled = false;
    bool checked = false;
};

struct context_menu_plan {
    bool visible = false;
    std::vector<context_menu_item> items;
};

// Pure menu-plan builder driven by ui_command_context
[[nodiscard]] context_menu_plan build_canvas_context_menu_plan(const ui_command_context& ctx);

}  // namespace oz::viewer
