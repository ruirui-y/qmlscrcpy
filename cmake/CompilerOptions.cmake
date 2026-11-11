# 编译选项做成 INTERFACE 目标：谁链接谁继承。
# 不在根上写 add_compile_options，否则以后新增的子目录会被悄悄污染。
add_library(qs_options INTERFACE)

# 目标语言标准。选 17 不选 20：Qt 5.15 的头在 /std:c++20 下偶发告警，收益不值这个风险
target_compile_features(qs_options INTERFACE cxx_std_17)

if(MSVC)
    target_compile_options(qs_options INTERFACE
        /W4                 # 比默认 /W3 多一层；/WX 留给 CI，先别让本地开发被警告卡住
        /permissive-        # 两阶段名字查找，提前暴露不合标准的写法
        /utf-8              # 源码 + 执行字符集都按 UTF-8，中文注释不再报 C4819
        /Zc:__cplusplus     # 让 __cplusplus 反映真实的 /std 值，而不是永远 199711L
        /wd4100             # 未使用的形参：Qt 虚函数签名固定，这警告没有信息量
    )
    target_compile_definitions(qs_options INTERFACE
        NOMINMAX            # 挡住 windows.h 里 min/max 两个宏
        WIN32_LEAN_AND_MEAN
        UNICODE             # 不定义的话 QString::fromLocal8Bit 在中文系统上会乱码
        _UNICODE
    )
else()
    target_compile_options(qs_options INTERFACE -Wall -Wextra -Wpedantic)
endif()
