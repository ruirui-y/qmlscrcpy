# 打包时把控制台窗口去掉；开发期留着，qDebug 才能看到
option(QS_HIDE_CONSOLE "去掉控制台窗口（发布版打开）" OFF)
set(qs_subsystem "")
if(QS_HIDE_CONSOLE)
    set(qs_subsystem WIN32)
endif()

# qrc 放在仓库根：它引用的是 ../qml/ 下的文件
add_executable(QmlScrcpy ${qs_subsystem}
    main.cpp
    ${PROJECT_SOURCE_DIR}/resources/app.rc
    app_settings.cpp
    app_settings.h
    device_controller.cpp
    device_controller.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/video_item.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/video_item.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml.qrc
)

# 版本号从 project() 传进来，避免 C++ 里再手写一遍
target_compile_definitions(QmlScrcpy PRIVATE
    QS_APP_VERSION="${PROJECT_VERSION}"
)

target_include_directories(QmlScrcpy PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(QmlScrcpy PRIVATE
    qs_options
    qsc_base
    qsc_session
    qsc_stream
    qsc_input
    Qt5::Quick
    Qt5::QuickControls2
)

# 多配置生成器会在 bin 下再套一层 Debug/，实际产物在 bin/<Config>/
set_target_properties(QmlScrcpy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# 运行时部署：把 Qt DLL 和 QML 模块一起拷到 exe 旁边，双击就能跑。
#
# 两件事必须做对，否则启动就是一堆 "module QtQuick is not installed"：
#
# 1. --qmldir 必须给。不给的话 windeployqt 只拷 DLL 不拷 qml 模块树；而一旦
#    Qt5Core.dll 躺在 exe 旁边，Qt 就按"已部署布局"去 <exe目录>/qml 找导入路径，
#    不会再回 Qt 安装目录找 → 所有 QML 模块都"没装"。
#
# 2. PATH 必须把 5.15.2 顶到最前面。windeployqt 是按 DLL 名字在 PATH 里找依赖的，
#    本机 PATH 第一个是 Qt 5.14.2（C:\Users\Mars\QT5.14.2\...），不顶掉就会把 5.14 的
#    插件拷进来，启动报"无法定位程序输入点"。
#
# 具体怎么改 PATH 放在 cmake/DeployQt.cmake 里做：在那行 add_custom_command 里直接写
# ${ENV P A T H} 会被 CMake 当列表按分号拆开，整条命令就散架了。
# ---------------------------------------------------------------------------
option(QS_DEPLOY_RUNTIME "构建后执行 windeployqt" ON)
if(WIN32 AND QS_DEPLOY_RUNTIME)
    add_custom_command(TARGET QmlScrcpy POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DQS_QT_ROOT=${QS_QT_ROOT}"
            # 多配置生成器拿不到 CMAKE_BUILD_TYPE，只能用 genex；两个拼起来保证永远只有一个开关
            "-DQS_DEPLOY_MODE=$<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>"
            "-DQS_QML_DIR=${PROJECT_SOURCE_DIR}/qml"
            "-DQS_BINARY=$<TARGET_FILE:QmlScrcpy>"
            -P "${PROJECT_SOURCE_DIR}/cmake/DeployQt.cmake"
        COMMENT "部署 Qt 运行库 + QML 模块"
        VERBATIM
    )
endif()

# scrcpy-server 跟着 exe 走：连接层默认按 applicationDirPath()/scrcpy-server 找它
add_custom_command(TARGET QmlScrcpy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/third_party/scrcpy-server"
        "$<TARGET_FILE_DIR:QmlScrcpy>/scrcpy-server"
    COMMENT "拷贝 scrcpy-server 到输出目录"
)

qs_deploy_ffmpeg(QmlScrcpy)
