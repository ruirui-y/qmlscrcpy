# 由 src/CMakeLists.txt 的 POST_BUILD 调用：
#   cmake -DQS_QT_ROOT=... -DQS_DEPLOY_MODE=--debug -DQS_QML_DIR=... -DQS_BINARY=... -P DeployQt.cmake
#
# 为什么不在 add_custom_command 里直接写 cmake -E env：
# 生成命令行时 $ENV{PATH} 里的分号会被 CMake 当成列表分隔符，整条 PATH 被拆成几十个参数，
# -E env 就把其中一段路径当成了要执行的命令（报错长得像 "pwsh.exe 不是内部或外部命令"）。
# 放在脚本里改环境变量没有这个问题，写法也简单。

set(ENV{PATH} "${QS_QT_ROOT}/bin;$ENV{PATH}")

execute_process(
    COMMAND "${QS_QT_ROOT}/bin/windeployqt.exe"
            --no-translations
            ${QS_DEPLOY_MODE}                       # --debug 或 --release
            --qmldir "${QS_QML_DIR}"                # 不给这个就只拷 DLL，不拷 qml 模块树
            "${QS_BINARY}"
    RESULT_VARIABLE deploy_result
)

if(NOT deploy_result EQUAL 0)
    message(FATAL_ERROR "windeployqt 失败，退出码 ${deploy_result}")
endif()

# 部署完必须能在 exe 旁边看到 QML 模块树。
# 注意 windeployqt 是平铺拷的：QtQuick.2/、QtQuick/Controls.2/ 直接躺在 exe 旁边，
# 不放在 qml/ 子目录里，所以这里要递归找而不是只看 qml/。
# 少了这一步，构建是"成功"的，启动才报一堆 "module QtQuick is not installed"。
get_filename_component(binary_dir "${QS_BINARY}" DIRECTORY)
file(GLOB_RECURSE deployed_qmldir "${binary_dir}/QtQuick*/qmldir" "${binary_dir}/QtQml*/qmldir")
if(NOT deployed_qmldir)
    message(FATAL_ERROR "QML 模块没被部署到 ${binary_dir}，检查 --qmldir 是否指到了 qml 目录")
endif()

list(LENGTH deployed_qmldir qml_module_count)
message(STATUS "部署完成：${qml_module_count} 个 qml 模块已在 ${binary_dir}")
