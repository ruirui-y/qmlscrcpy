#include "ui/d3d12_nv12_convert.h"

#include <QDebug>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace qsc::ui
{

namespace
{

// 全屏三角形 + NV12 转 BT.601 有限范围。顶点着色器用 SV_VertexID 生成三个角，不用顶点缓冲
constexpr char kShaderSource[] = R"(
Texture2D<float> texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samp      : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // id = 0/1/2 生成 (-1,-1) (3,-1) (-1,3)，铺满整个视口
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    // 有限范围 BT.601：Y 先减 16/255，色度减 128/255
    float y  = texY.Sample(samp, i.uv).r - 0.0625;
    float2 uv = texUV.Sample(samp, i.uv) - 0.5;
    float3 rgb = float3(y + 1.596 * uv.y, y - 0.391 * uv.x - 0.813 * uv.y, y + 2.018 * uv.x);
    return float4(saturate(rgb), 1.0);
}
)";

// 编译一段着色器：失败时把编译器吐的错误原样带出来
bool Compile(const char *entry, const char *target, ComPtr<ID3DBlob> &blob, QString *error)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT ret = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr, nullptr,
                                   entry,                                                                  // 入口函数
                                   target,                                                                 // 目标档次
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3,                                         // 优化
                                   0,                                                                      // 没有额外标志
                                   blob.GetAddressOf(),                                                    // 出：字节码
                                   errors.GetAddressOf());                                                  // 出：错误信息
    if (FAILED(ret))
    {
        const char *text = errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "无详细信息";
        *error = QStringLiteral("着色器编译失败（%1）：%2").arg(QLatin1String(entry), QLatin1String(text));
        return false;
    }
    return true;
}

constexpr int kSrvCount = 2;                                                                    // Y 和 UV 两个描述符

} // namespace

// 设备、队列、围栏、描述符堆这些跟着 Initialize 建；析构时一起放掉
struct D3d12Nv12Convert::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;                                                          // 两个 SRV
    ComPtr<ID3D12DescriptorHeap> rtv_heap;                                                      // 一个 RTV，转色目标
    quint32 descriptor_size = 0;
    HANDLE event = nullptr;
    quint64 fence_value = 0;

    void *last_nv12 = nullptr;                                                                  // 上次建视图的资源，换资源就重建
    int last_subresource = -1;
};

D3d12Nv12Convert::~D3d12Nv12Convert()
{
    Release();
}

// 建设备相关对象：命令队列、分配器、命令列表、围栏、描述符堆
bool D3d12Nv12Convert::PrepareQueue(QString *error)
{
    Impl &d = *impl_;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;                                           // 要录资源屏障，不能用 COPY 队列
    if (FAILED(d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(d.queue.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色命令队列失败");
        return false;
    }
    if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(d.allocator.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色命令分配器失败");
        return false;
    }
    if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(d.list.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色命令列表失败");
        return false;
    }
    d.list->Close();                                                                            // 建出来就是打开状态，先关掉，用的时候再 Reset
    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.fence.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色围栏失败");
        return false;
    }
    d.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (d.event == nullptr)
    {
        *error = QStringLiteral("建转色事件失败");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kSrvCount;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;                                 // 要给着色器用
    if (FAILED(d.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(d.heap.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色描述符堆失败");
        return false;
    }
    d.descriptor_size = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    if (FAILED(d.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(d.rtv_heap.ReleaseAndGetAddressOf()))))
    {
        *error = QStringLiteral("建转色 RTV 堆失败");
        return false;
    }
    return true;
}

// 建根签名和渲染管线。两个 SRV 走描述符表，采样器用静态采样器省一个堆
bool D3d12Nv12Convert::EnsurePipeline(QString *error)
{
    if (pipeline_ != nullptr)
    {
        return true;
    }
    Impl &d = *impl_;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvCount;                                                           // Y 和 UV
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;                                           // 缩放靠它，画布比帧小
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &param;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> error_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          signature_blob.GetAddressOf(), error_blob.GetAddressOf())))
    {
        const char *text = error_blob != nullptr ? static_cast<const char *>(error_blob->GetBufferPointer()) : "";
        *error = QStringLiteral("序列化根签名失败：%1").arg(QLatin1String(text));
        return false;
    }
    if (FAILED(d.device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
                                             IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature **>(&root_signature_)))))
    {
        *error = QStringLiteral("建根签名失败");
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    if (!Compile("VSMain", "vs_5_0", vs, error) || !Compile("PSMain", "ps_5_0", ps, error))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = static_cast<ID3D12RootSignature *>(root_signature_);
    desc.VS.pShaderBytecode = vs->GetBufferPointer();
    desc.VS.BytecodeLength = vs->GetBufferSize();
    desc.PS.pShaderBytecode = ps->GetBufferPointer();
    desc.PS.BytecodeLength = ps->GetBufferSize();
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;                                       // 全屏三角形不挑正反面
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;                                            // 由 Convert 里按目标核对
    desc.SampleDesc.Count = 1;

    const HRESULT ret = d.device->CreateGraphicsPipelineState(&desc,
                                                              IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState **>(&pipeline_)));
    if (FAILED(ret))
    {
        *error = QStringLiteral("建转色管线失败（HRESULT 0x%1）").arg(ret, 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

// 给 Y 和 UV 两个平面各建一个 SRV：同一个资源，靠 PlaneSlice 区分
bool D3d12Nv12Convert::EnsureViews(void *nv12, int subresource, QString *error)
{
    Impl &d = *impl_;
    if (d.last_nv12 == nv12 && d.last_subresource == subresource)
    {
        return true;                                                                            // 同一个资源同一个层，视图不用重建
    }

    ComPtr<ID3D12Resource> resource;
    resource.Attach(static_cast<ID3D12Resource *>(nv12));
    const D3D12_RESOURCE_DESC resource_desc = resource->GetDesc();

    const auto make_view = [&](UINT index, DXGI_FORMAT format, UINT plane_slice)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MostDetailedMip = 0;
        view.Texture2D.MipLevels = 1;
        view.Texture2D.PlaneSlice = plane_slice;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = d.heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * d.descriptor_size;
        d.device->CreateShaderResourceView(resource.Get(), &view, handle);
    };

    // NV12：平面 0 是 Y（单通道），平面 1 是 UV（两通道）
    make_view(0, DXGI_FORMAT_R8_UNORM, 0);
    make_view(1, DXGI_FORMAT_R8G8_UNORM, 1);

    if (resource_desc.Format != DXGI_FORMAT_NV12)
    {
        *error = QStringLiteral("转色输入不是 NV12（格式 %1）").arg(static_cast<int>(resource_desc.Format));
        resource.Detach();                                                                      // 还回所有权，别在这里释放
        return false;
    }

    resource.Detach();
    d.last_nv12 = nv12;
    d.last_subresource = subresource;
    return true;
}

// 等我们自己的围栏：转色命令跑完才能让 RHI 采样目标纹理
bool D3d12Nv12Convert::WaitFence(QString *error)
{
    Impl &d = *impl_;
    const quint64 value = ++d.fence_value;
    if (FAILED(d.queue->Signal(d.fence.Get(), value)))
    {
        *error = QStringLiteral("转色提交后发信号失败");
        return false;
    }
    if (d.fence->GetCompletedValue() < value)
    {
        if (FAILED(d.fence->SetEventOnCompletion(value, d.event)))
        {
            *error = QStringLiteral("等转色围栏失败");
            return false;
        }
        ::WaitForSingleObject(d.event, INFINITE);
    }
    return true;
}

// 初始化：拿到设备，建队列与管线
bool D3d12Nv12Convert::Initialize(void *device, QString *error)
{
    if (device == nullptr)
    {
        *error = QStringLiteral("转色没有设备");
        return false;
    }
    if (impl_ != nullptr)
    {
        return true;
    }
    impl_ = new Impl();
    device_ = device;
    impl_->device = static_cast<ID3D12Device *>(device);
    reinterpret_cast<IUnknown *>(device_)->AddRef();                                            // 跟着本对象活

    if (!PrepareQueue(error) || !EnsurePipeline(error))
    {
        Release();
        return false;
    }
    return true;
}

// 一趟 GPU 转色：等解码围栏 → 采样 NV12 写进目标纹理 → 等自己围栏
bool D3d12Nv12Convert::Convert(void *nv12, int subresource, void *decode_fence, quint64 decode_fence_value,
                              void *target, int width, int height, QString *error)
{
    if (!Ready() || nv12 == nullptr || target == nullptr)
    {
        *error = QStringLiteral("转色尚未就绪");
        return false;
    }
    Impl &d = *impl_;
    if (!EnsureViews(nv12, subresource, error))
    {
        return false;
    }

    ComPtr<ID3D12Resource> input;
    input.Attach(static_cast<ID3D12Resource *>(nv12));
    ComPtr<ID3D12Resource> output;
    output.Attach(static_cast<ID3D12Resource *>(target));

    if (FAILED(d.allocator->Reset()) || FAILED(d.list->Reset(d.allocator.Get(), static_cast<ID3D12PipelineState *>(pipeline_))))
    {
        *error = QStringLiteral("重置转色命令列表失败");
        input.Detach();
        output.Detach();
        return false;
    }

    // 第一步：资源状态切进切出，顺序不能反
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = input.Get();
    barriers[0].Transition.Subresource = static_cast<UINT>(subresource);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = output.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;              // RHI 平时就放在这个状态
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    d.list->ResourceBarrier(2, barriers);

    // 第二步：绑目标、视口、描述符堆，画一个全屏三角形
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = output->GetDesc().Format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d.device->CreateRenderTargetView(output.Get(), &rtv_desc, rtv);

    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    d.list->RSSetViewports(1, &viewport);

    const D3D12_RECT scissor = {0, 0, width, height};
    d.list->RSSetScissorRects(1, &scissor);
    d.list->SetGraphicsRootSignature(static_cast<ID3D12RootSignature *>(root_signature_));
    d.list->SetDescriptorHeaps(1, d.heap.GetAddressOf());
    d.list->SetGraphicsRootDescriptorTable(0, d.heap->GetGPUDescriptorHandleForHeapStart());
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);                                                          // 全屏三角形，没有顶点缓冲

    // 第三步：把两个资源的状态切回去，否则 RHI 采样时会报 state mismatch
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;                            // 还给 FFmpeg
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;             // 还给 RHI
    d.list->ResourceBarrier(2, barriers);

    // 第四步：先让 GPU 等解码围栏，再提交，最后 CPU 等我们自己的围栏
    if (decode_fence != nullptr)
    {
        d.queue->Wait(static_cast<ID3D12Fence *>(decode_fence), decode_fence_value);
    }
    if (FAILED(d.list->Close()))
    {
        input.Detach();
        output.Detach();
        *error = QStringLiteral("关闭转色命令列表失败");
        return false;
    }
    ID3D12CommandList *lists[] = {d.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);

    input.Detach();
    output.Detach();

    return WaitFence(error);
}
// 释放所有设备对象（设备自己的引用也要还）
void D3d12Nv12Convert::Release()
{
    if (impl_ != nullptr)
    {
        if (impl_->event != nullptr)
        {
            ::CloseHandle(impl_->event);
            impl_->event = nullptr;
        }
        delete impl_;
        impl_ = nullptr;
    }
    if (pipeline_ != nullptr)
    {
        reinterpret_cast<IUnknown *>(pipeline_)->Release();
        pipeline_ = nullptr;
    }
    if (root_signature_ != nullptr)
    {
        reinterpret_cast<IUnknown *>(root_signature_)->Release();
        root_signature_ = nullptr;
    }
    if (device_ != nullptr)
    {
        reinterpret_cast<IUnknown *>(device_)->Release();
        device_ = nullptr;
    }
}

} // namespace qsc::ui
