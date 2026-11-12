# 版本检查：只放纯逻辑（版本比较），网络部分留在 src/，这样比较规则能单独单测
add_library(qsc_update STATIC
    version_compare.cpp
    version_compare.h
)

target_include_directories(qsc_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(qsc_update PUBLIC qs_options Qt5::Core)