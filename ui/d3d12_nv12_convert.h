#pragma once

// NV12 → RGBA 的 GPU 内转色。
// 解码纹理和 Qt 的 RGBA 纹理在同一台 D3D12 设备上，这里用自己的命令队列录一趟全屏绘制，
// 把 NV12 两个平面采样成 RGBA 写进 Qt 那张纹理，全程不过 CPU。
//
// 同步：先在 GPU 上等 FFmpeg 的解码围栏，提交后 CPU 再等我们自己的围栏（约 0.1~0.3ms）。
// 着色器是运行时用 D3DCompile 编的，免得构建期再引入 fxc/dxc。

#include <QString>
#include <QtGlobal>

namespace qsc::ui
{

class D3d12Nv12Convert
{
public:
    ~D3d12Nv12Convert();

    bool Initialize(void *device, QString *error);                                              // device = ID3D12Device*
    void Release();

    // 把 nv12 的某个子资源画进 target（ID3D12Resource*，格式由它自己决定）。
    // 返回 true 时 GPU 已经写完（CPU 已经等过围栏），调用方可以直接让 RHI 采样 target
    bool Convert(void *nv12, int subresource, void *decode_fence, quint64 decode_fence_value,
                 void *target, int width, int height, QString *error);

    bool Ready() const { return device_ != nullptr && pipeline_ != nullptr; }

private:
    bool EnsurePipeline(QString *error);
    bool EnsureViews(void *nv12, int subresource, QString *error);
    bool PrepareQueue(QString *error);                                                          // 建设备相关对象
    bool WaitFence(QString *error);

    struct Impl;
    Impl *impl_ = nullptr;                                                                      // 定义在 cpp 里，头文件不引 d3d12
    void *device_ = nullptr;                                                                    // ID3D12Device*，不持有引用
    void *pipeline_ = nullptr;                                                                  // ID3D12PipelineState*
    void *root_signature_ = nullptr;                                                            // ID3D12RootSignature*
};

} // namespace qsc::ui
