# Qt 渲染插口演进：从场景图到 RHI

这篇讲清楚三件事：

```
① QML 里一个 Rectangle 是怎么变成屏幕像素的（场景图）
② Qt 为什么给应用留"插口"，插口长什么样（FBO → RhiItem）
③ 我们项目在这两代插口上分别踩了什么
```

第 ① 部分不懂的话，第 ② 部分会越看越玄。

---

## 一、Qt Quick 怎么把 QML 画出来

### 两遍：先建对象，再发射

```
① 加载 QML（一次）
   Rectangle { width: 100; color: "red" }
        ↓
   创建 QQuickRectangle 对象，记下属性
        ↓
   （这一步完全不碰 GPU）

② 每次需要重绘（每秒几十次）
   遍历对象树 → 生成"绘制条目"
        ↓
   ┌──────────────────────────────┐
   │ 画矩形 位置(100,200) 尺寸100×50 颜色红 │
   │ 画文字 "hello"                        │
   │ 画自定义条目 ← ★ 我们的视频在这里      │
   └──────────────────────────────┘
        ↓
   翻译成 GPU 命令 → 执行 → 交换缓冲
```

这套机制叫**场景图（Scene Graph）**。Qt 负责"遍历 + 翻译 + 提交"，你只负责描述。

### 为什么需要"插口"

视频不是矩形，没法用 `Rectangle` 表达。Qt 留了扩展点：

```
Qt 的场景图遍历到"自定义条目"时
   ↓
调用你注册的回调（render / synchronize）
   ↓
你在里面画你的东西
```

**这叫控制反转**——不是你调 Qt，是 Qt 调你。所以你必须按它的约定写：什么时候被调、上下文是否就绪、资源在哪创建在哪释放。

---

## 二、第一代插口：`QQuickFramebufferObject`（Qt5）

### FBO 是什么

```
Framebuffer（帧缓冲）= 一块装像素的内存
  平时：GPU 画到"窗口的帧缓冲" → 交换 → 屏幕显示
  FBO ：一块不显示的帧缓冲，可以当纹理用
```

类比：

```
窗口帧缓冲 = 舞台（观众看得到）
FBO        = 后台画布
             你在后台画好，再把画布举到台上
```

### 完整流程

```
① QML 里 VideoItem 占 400×900 的位置
        ↓
② VideoItem 声明"我要一块 400×900 的离屏画布"
        ↓
③ 渲染时 Qt 把这块画布交给你：
      render() 被调用，FBO 是当前渲染目标，GL 上下文当前化
        ↓
④ 你在上面画：
      glTexImage2D(NV12 数据)    上传两平面纹理
      glUseProgram(转色着色器)    告诉 GPU 怎么把 YUV 变 RGB
      glDrawArrays(画一个矩形)    铺满画布
        ↓
⑤ Qt 把这块画布当普通纹理，贴到窗口的 (200, y) 位置
        ↓
⑥ 交换缓冲 → 屏幕
```

### 两个关键约定

| 方法 | 谁调 | 干什么 |
|---|---|---|
| `synchronize(QQuickFramebufferObject *item)` | GUI 线程 | 把数据从 QML 对象拷到渲染器（此时**没有** GL 上下文，不能调 gl*） |
| `render()` | 渲染线程 | 真正画，此时 **GL 上下文当前化**，可以随便调 gl* |

### 优点 / 缺点

```
优点：环境简单，render() 里就是 OpenGL，想怎么画怎么画
缺点：绑死 OpenGL
      Qt6 要支持 Metal / D3D12 / Vulkan 时，这套用不了
```

---

## 三、Qt6 的方案：RHI

### RHI 是什么

**Rendering Hardware Interface**——一层统一接口，底下一堆后端。

```
        你的代码（只写一份）
              ↓
     ┌──── QRhi（统一接口）────┐
     │  newTexture() / newBuffer() │
     │  beginFrame() / endFrame()  │
     │  nextCommandBuffer()        │
     └──────────┬───────────────┘
                ↓ 运行时选一个
   D3D11 │ D3D12 │ Vulkan │ Metal │ OpenGL
```

类比：

```
OpenGL 时代 = 你要学每国语言，挨个跟每个国家的人说话
RHI 时代    = 雇一个翻译，你说中文，他翻成当地语言
```

### 五个必须认识的名词

| 名词 | 类比 | 作用 |
|---|---|---|
| `QRhi` | 工厂 + 调度员 | 建资源、开始/结束一帧 |
| **Command buffer** | 一张"待办清单" | 你把"画这个、拷那个"写上去 |
| **Render pass** | 一次"开工到收工" | 规定这轮画到哪个目标、要不要清屏 |
| **Pipeline** | 一条固定装配线 | 着色器 + 顶点格式 + 混合模式打包 |
| **Shader（QSB）** | 装配线上的工人 | 每个像素算什么颜色（Qt 统一编译成 `.qsb`） |

一帧的骨架：

```cpp
rhi->beginFrame(swapchain);
QRhiCommandBuffer *cb = rhi->nextCommandBuffer();
cb->beginPass(renderTarget, clearColor, 1.0f, 0);
cb->setGraphicsPipeline(pipeline);
cb->setShaderResources();
cb->draw(4);                       // 一个矩形 = 两个三角 = 4 顶点
cb->endPass();
rhi->endFrame();
```

### 第二代插口：`QQuickRhiItem`

```
QQuickFramebufferObject（Qt5）        QQuickRhiItem（Qt6.6+）
──────────────────────────────       ──────────────────────────────
createRenderer()                      createRenderer()
Renderer::render()                    Renderer::render(QRhiCommandBuffer *cb)
                                       ↑ 多给你一张"待办清单"
只能用 OpenGL                          后端由 Qt 决定
```

**注意：画布还是 Qt 给的**（render target），只是画笔从"直接调 gl*"变成了"往清单上写命令"。

### 我们踩的 8 个坑（详见 [GPU渲染改造](../03-视频与解码/GPU渲染改造.md)）

| 坑 | 现象 |
|---|---|
| 1 格式必须 RGBA | 用 BGRA 会黑屏/错色 |
| 2 RhiItem 不替你开 render pass | 不调 beginPass 什么都不画 |
| 3 不设视口 | 全黑 |
| 4 顶点缓冲要在 `initialize(cb)` 里上传 | 在 render 里建会崩 |
| 5 QSB 着色器要随 exe 走 | 少了 `.qsb` 就白屏 |
| 6 `uploadTexture(tex, QByteArray)` 这个重载不存在 | 编译不过 |
| 7 顶点缓冲建了不等于传了 | 画面错位 |
| 8 RHI 头文件不在默认 include 路径 | 编译不过 |

---

## 四、两代插口对照

| 维度 | `QQuickFramebufferObject` | `QQuickRhiItem` |
|---|---|---|
| 版本 | Qt5 起 | Qt6.6 起 |
| 给你什么 | 一块 FBO + OpenGL 上下文 | render target + 命令清单 |
| 你的画笔 | `gl*` 函数 | `QRhiCommandBuffer` |
| 图形 API | 只能 OpenGL | D3D11 / D3D12 / Vulkan / Metal / GL |
| 上下文谁管 | Qt（渲染线程唯一那个） | Qt（RHI 内部设备） |
| 我们项目 | **master 现在用它** | exp/zero-copy 试过，已回退 |

---

## 五、纠正两个说法

### ① "Qt6 不给画布了"

```
错。画布（render target）还是 Qt 给的。
变的只是"怎么画"：从直接调 GL 变成写命令清单。
```

### ② "纹理必须是 Qt 创建的"

```
错。纹理只要能在 Qt 当前后端访问即可：
  - OpenGL 分支：我们一直自己 glGenTextures，Qt 照样能采样
  - D3D11 分支：必须在同一个设备上，或者共享句柄 + 同步
```

---

## 六、为什么我们最后回到 FBO

```
exp/zero-copy 的目标：把 D3D11 解码纹理直接给 RHI 用（零拷贝）
失败原因：Qt RHI 的 D3D11 后端和 FFmpeg 抢同一个即时上下文 → TDR
（细节见 [零拷贝为什么失败](零拷贝为什么失败.md)）
```

回到 FBO 之后：

```
解码 → 读回内存 → glTexImage2D 上传 → GL 着色器转色
绕了一圈，但每一层都在自己可控的世界里，稳。
```

---

## 七、自测三问

**Q1** `synchronize()` 里能调 `glTexImage2D` 吗？

```
A：不能。synchronize 在 GUI 线程跑，此时 GL 上下文不在这个线程上。
   上传纹理要放到 render()。
```

**Q2** `QQuickRhiItem` 和 `QQuickFramebufferObject` 的本质差别是什么？

```
A：给你"直接调图形 API"还是"写命令清单"。
   前者绑死 OpenGL，后者由 Qt 选后端。
```

**Q3** 为什么我们宁可每帧多搬 2ms 也不用 RHI 零拷贝？

```
A：RHI 的 D3D11 后端不肯让出即时上下文的控制权，
   我们插进去的命令和它的命令必然打架，代价是设备丢失（TDR）。
```

---

## 八、一句话总结

```
Qt5：给你一块 OpenGL 画布（简单、绑死 GL）
Qt6：给你一张命令清单（跨平台、但资源归 Qt 管）
插口没变的是"控制反转"——何时画、用什么画，Qt 说了算。
```
