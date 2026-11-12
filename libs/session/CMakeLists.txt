# 连接层：把 scrcpy-server 推上设备、拉起来、把视频和控制两条 socket 建好。
# 内部全是阻塞的 waitFor*，必须在工作线程里调用。
add_library(qsc_session STATIC
    server_args.cpp
    server_session.cpp
    server_args.h
    server_session.h
)

target_include_directories(qsc_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_session PUBLIC qsc_base qsc_adb qsc_protocol Qt5::Network)
