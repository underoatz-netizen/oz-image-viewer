// oz/viewer/context_menu.cpp - testable pure context-menu plan seam implementation (Stage 9.2).
#include "context_menu.hpp"

namespace oz::viewer {

context_menu_plan build_canvas_context_menu_plan(const ui_command_context& ctx) {
    context_menu_plan plan;

    // Reject benchmark mode or missing presented image
    if (ctx.benchmark_mode || !ctx.have_image) {
        plan.visible = false;
        plan.items.clear();
        return plan;
    }

    plan.visible = true;

    // 1. Fit to Window (ID 302)
    const auto fit_st = derive_command_state(ui_command_id::view_fit, ctx);
    context_menu_item fit_item;
    fit_item.type = context_menu_item_type::command;
    fit_item.command_id = static_cast<int>(ui_command_id::view_fit);
    fit_item.text = L"Fit to Window\tF";
    fit_item.enabled = fit_st.enabled;
    fit_item.checked = fit_st.checked;
    plan.items.push_back(std::move(fit_item));

    // 2. Actual Size (ID 301)
    const auto actual_st = derive_command_state(ui_command_id::view_actual_size, ctx);
    context_menu_item actual_item;
    actual_item.type = context_menu_item_type::command;
    actual_item.command_id = static_cast<int>(ui_command_id::view_actual_size);
    actual_item.text = L"Actual Size\t1";
    actual_item.enabled = actual_st.enabled;
    actual_item.checked = actual_st.checked;
    plan.items.push_back(std::move(actual_item));

    // Ensure fit and actual size checks are strictly exclusive
    if (plan.items[0].checked && plan.items[1].checked) {
        // In the impossible event both were checked, default to fit
        plan.items[1].checked = false;
    }

    // 3. Separator
    context_menu_item sep_item;
    sep_item.type = context_menu_item_type::separator;
    plan.items.push_back(std::move(sep_item));

    // 4. Copy Image (ID 108)
    const auto copy_img_st = derive_command_state(ui_command_id::file_copy_image, ctx);
    context_menu_item copy_img_item;
    copy_img_item.type = context_menu_item_type::command;
    copy_img_item.command_id = static_cast<int>(ui_command_id::file_copy_image);
    copy_img_item.text = L"Copy Image\tCtrl+C";
    copy_img_item.enabled = copy_img_st.enabled;
    copy_img_item.checked = false;
    plan.items.push_back(std::move(copy_img_item));

    // 5. Copy File (ID 106)
    const auto copy_file_st = derive_command_state(ui_command_id::file_copy, ctx);
    context_menu_item copy_file_item;
    copy_file_item.type = context_menu_item_type::command;
    copy_file_item.command_id = static_cast<int>(ui_command_id::file_copy);
    copy_file_item.text = L"Copy File";
    copy_file_item.enabled = copy_file_st.enabled;
    copy_file_item.checked = false;
    plan.items.push_back(std::move(copy_file_item));

    // 6. Copy File Path (ID 107)
    const auto copy_path_st = derive_command_state(ui_command_id::file_copy_path, ctx);
    context_menu_item copy_path_item;
    copy_path_item.type = context_menu_item_type::command;
    copy_path_item.command_id = static_cast<int>(ui_command_id::file_copy_path);
    copy_path_item.text = L"Copy File Path";
    copy_path_item.enabled = copy_path_st.enabled;
    copy_path_item.checked = false;
    plan.items.push_back(std::move(copy_path_item));

    // 7. Open File Location (ID 105)
    const auto open_loc_st = derive_command_state(ui_command_id::file_open_location, ctx);
    context_menu_item open_loc_item;
    open_loc_item.type = context_menu_item_type::command;
    open_loc_item.command_id = static_cast<int>(ui_command_id::file_open_location);
    open_loc_item.text = L"Open File Location";
    open_loc_item.enabled = open_loc_st.enabled;
    open_loc_item.checked = false;
    plan.items.push_back(std::move(open_loc_item));

    return plan;
}

}  // namespace oz::viewer
