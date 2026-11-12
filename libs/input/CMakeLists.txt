# 鼠标/键盘注入：坐标换算 + 控制消息拼装。
# 纯逻辑，不碰界面也不碰设备，所以能脱机单测。
add_library(qsc_input STATIC
    coordinate_mapper.cpp
    key_controller.cpp
    keycode_table.cpp
    touch_controller.cpp
    coordinate_mapper.h
    key_controller.h
    keycode_table.h
    touch_controller.h
)

target_include_directories(qsc_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_input PUBLIC qsc_protocol qs_options Qt5::Core)
