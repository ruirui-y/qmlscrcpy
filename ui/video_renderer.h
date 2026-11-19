#pragma once

// 视频渲染器：GL 上传、YUV/BGRA 两套着色器、D3D11 纹理的 interop，全在这里。
// 从 video_item.cpp 拆出来，那边只留"画布"本身的生命周期和事件。

#include <QByteArray>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QQuickFramebufferObject>
#include <QSize>

#include <windows.h>

#include "ui/video_item.h"

namespace qsc::ui
{



// WGL_NV_DX_interop2 的函数指针，Qt 不提供封装，只能自己从 GL 上下文取
using PfnWglDXOpenDeviceNV = void *(WINAPI *)(void *);
using PfnWglDXCloseDeviceNV = BOOL (WINAPI *)(void *);
using PfnWglDXSetResourceShareHandleNV = BOOL (WINAPI *)(void *, HANDLE);
using PfnWglDXRegisterObjectNV = void *(WINAPI *)(void *, void *, GLuint, GLenum, GLenum);
using PfnWglDXUnregisterObjectNV = BOOL (WINAPI *)(void *, void *);
using PfnWglDXLockObjectsNV = BOOL (WINAPI *)(void *, GLint, void **);
using PfnWglDXUnlockObjectsNV = BOOL (WINAPI *)(void *, GLint, void **);

constexpr GLenum kWglAccessReadOnlyNV = 0x0000;                                                 // WGL_ACCESS_READ_ONLY_NV


class YuvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    void synchronize(QQuickFramebufferObject *item) override;
    void render() override;

private:
    void EnsureProgram();
    void EnsureInterop();                                                                       // 加载 wglDX* 扩展并打开设备
    bool RegisterNative();                                                                      // 把 D3D11 纹理注册成 GL 纹理（按指针缓存）
    void DrawQuad(QOpenGLFunctions *gl, float width, float height);
    void DrawQuadRaw(QOpenGLFunctions *gl, float width, float height);                          // 原生纹理用：直接调 GL，绕开 Qt 的属性封装

    QSize item_size_;                                                                           // 画布（FBO）尺寸，每次 synchronize 更新
    FrameTracePtr trace_;                                                                       // 当前这一帧的事件记录，render() 回报耗时用
    VideoItem *item_ = nullptr;                                                                 // synchronize 时记住画布指针，render() 回报耗时要它
    bool keep_aspect_ = false;

    QByteArray planes_[3];                                                                      // 三个平面的紧凑像素副本（NV12 只用前两个）
    int widths_[3] = {0, 0, 0};
    int heights_[3] = {0, 0, 0};
    QSize frame_size_;                                                                          // 副本对应的帧尺寸
    bool nv12_ = false;                                                                         // 这一帧是不是 NV12（硬解下载的格式）
    bool dirty_ = false;                                                                        // 有没上传的新像素

    // 零拷贝：解码线程转出来的 BGRA 纹理，直接注册给 GL
    bool native_pending_ = false;                                                               // 这一轮要画原生纹理
    void *native_device_ = nullptr;                                                             // ID3D11Device*
    void *native_texture_ = nullptr;                                                            // ID3D11Texture2D*
    quint64 native_share_ = 0;                                                                  // 共享句柄
    QSize native_size_;

    GLuint textures_[3] = {0, 0, 0};
    QSize texture_sizes_[3];                                                                    // 每张纹理当前对应的平面尺寸，变了就重建
    bool texture_is_la_[3] = {false, false, false};                                              // 每张纹理当前是不是 LUMINANCE_ALPHA，格式变了必须重建
    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram nv12_program_;
    QOpenGLShaderProgram bgra_program_;                                                         // 原生 BGRA 纹理用：采样后交换 B/R
    QOpenGLShaderProgram *draw_program_ = &program_;                                            // DrawQuad 当前用哪套着色器
    bool program_ready_ = false;

    // wglDX* 扩展函数，只在渲染线程的 GL 上下文里用
    bool interop_ready_ = false;
    bool interop_tried_ = false;                                                                // 只加载一次，失败不再刷日志
    void *interop_device_ = nullptr;                                                            // wglDXOpenDeviceNV 的返回值
    PfnWglDXOpenDeviceNV dx_open_device_ = nullptr;
    PfnWglDXCloseDeviceNV dx_close_device_ = nullptr;
    PfnWglDXSetResourceShareHandleNV dx_set_share_handle_ = nullptr;
    PfnWglDXRegisterObjectNV dx_register_ = nullptr;
    PfnWglDXUnregisterObjectNV dx_unregister_ = nullptr;
    PfnWglDXLockObjectsNV dx_lock_ = nullptr;
    PfnWglDXUnlockObjectsNV dx_unlock_ = nullptr;
    struct Registered
    {
        void *texture = nullptr;                                                                // D3D11 纹理指针，用作缓存键
        GLuint gl_texture = 0;                                                                  // 对应的 GL 纹理名
        void *object = nullptr;                                                                 // wglDXRegisterObjectNV 的句柄
    };
    Registered registered_[4];
    int registered_count_ = 0;

};

} // namespace qsc::ui
