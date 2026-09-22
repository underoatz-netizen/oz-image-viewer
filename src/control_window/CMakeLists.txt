# ozbench_control_window - test-only subject for the external_visible_frame
# collector (blank-then-pattern window). Never used for real measurements.
add_executable(ozbench_control_window main.cpp)
target_link_libraries(ozbench_control_window PRIVATE user32 gdi32)