> **2026-10-25 备注：** 本文下列结论已失效或已实施：`NativeDX11` 已改名 `[NeuralRendering] NativeCapture` 且默认关闭；`Backup` / `BeforeBind` / `BeforeFinish` / `PSPixels` / `ControlMask` / `trust` 均已删除。以当前源码为准。

本次补充（2026-10-13）：所有测试条件块与无调用者的测试支持已清理，Release / Debug 构建及全部 4 个登记游戏部署完成。以下为历史检查记录，旧测试保留描述已失效；当前状态见 [最终报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-REPORT-2026-10-12-13.md)。

# DX11 工程检查与 DX12 游戏残留清理

日期：2026-10-12。对象：`DLSSNR_LiteDX11`。

## 后续修复状态（2026-10-12）

用户授权后已完成如下修复。**下方初次检查正文、行号和哈希保留为修复前快照；当前状态以本节为准。**

| 初次检查项 | 当前状态 |
| --- | --- |
| 1：输入锁 | 调用游戏 WndProc 前解锁；RouteKeyboard 先过滤无关消息，并避免递归调用时留下外层锁。同步迁入 DX12 的焦点刷新：安装子类后、打开面板及面板可见帧重新读取实际焦点。 |
| 2：失败重试参数泄漏 | 申请参数前要求对应 DestroyParameters 导出；失败重试释放自己的参数块，销毁失败则保留原状态、不继续申请。 |
| 3：风格切换工作纹理重建 | 同尺寸/格式保留 Context 工作纹理，只重建 Layer；每个 recording 独立持有其 Layer，保留 GPU 生命周期。 |
| 4：完整 NGX 输入时多余深度工作 | 已省掉帧尾候选备份/拷贝，同时正常重置统计和推进候选年龄。绘制/绑定追踪及清除前必要备份仍保留，以便 NGX 缺帧时回退；并未声称所有原生追踪开销消失。 |
| 5：忙帧销毁光流资源 | Bridge 在录制前返回 S_FALSE 时只使历史失效，保留纹理、状态记录与已上传噪声。已录制后发生错误仍完整 Reset，避免复用未提交录制的状态。 |
| 6：运行时 HLSL 编译 | 插件全部 14 个 shader entry 在配置阶段以 fxc 预编译；保留原优化与光流严格/IEEE 选项。ImGui 官方 DX11 后端自身的首次编译仍保留。 |
| feature 错误路径 | 不再丢失 handle：暂存 feature 和票据，在确认未执行（Discarded）或已完成时释放；Pending 和 Signal 失败不提前释放。模型 Release 失败写入日志。 |

修复仅发生在 DX11 工程，未修改 DX12 与参考工程。未启动游戏、创建测试窗口或运行 GPU 验证。修复验证完成后，按用户后续授权已部署到博德之门 3 与 `H:\Games\RiseOfEros`，部署记录见 `docs/DEPLOYMENTS.md`。

验证：Release 编译通过，无警告/错误；14 个离线 shader entry 编译成功；三个从 C++ 提取的 shader 源与原 Lite 逐字比较一致，color/flow HLSL 与原 Lite 哈希一致。临时 CPU 检查通过 Pending/Discarded 退役、只释放一次、风格切换 recording 对 Layer 的保留、Signal 失败不可当成完成四项检查，不接入发布构建、不导出测试接口。

日志：`build/dx11-fixes-build-20261012.log`、`build/dx11-fixes-final-build-20261012.log`、`build/dx11-cpu-retirement-check.log`。

当前修复版主体 SHA256：`0992D4C280E7E3A13C8946FB028B65CA82AAA6CB0CE4A4E445B6780091454E88`；博德之门 3 与 ROE 已部署主体与此哈希一致。

这些修改不能替代真实游戏运行验证，也不据此宣称旧有“丢失焦点”或“GPU 时间永久增加”现象已经彻底解决。模型 Release 本身若失败，当前仅记录错误；故障保护中的资源隔离、有限调试缓存保留没有强行删除。

---

## 初次检查快照

检查范围包括构建、启动/转发、工厂与交换链包装、DX11 NGX 捕获、原生深度捕获、共享资源桥接、模型初始化/释放、提交与资源保留、GPU 计时，以及窗口输入的同步。没有修改图像算法、面板布局或参考工程，没有启动游戏、运行 GPU 测试或部署。以下运行风险来自静态调用路径，尚未用真实游戏复现。

## 本轮已完成的清理

| 移除项 | 判断依据 | 涉及文件 |
| --- | --- | --- |
| GTA Enhanced 专项配置、进程判断、小交换链排除 | 专用于 DX12 游戏 `GTA5_Enhanced.exe`，本工程只包装 DX11 设备创建的交换链 | `runtime.cpp/.hpp`、`hooks.cpp` |
| 为上述分支传递的请求宽高与 HWND 自动尺寸解析 | 删除专项判断后没有消费者；保留原生创建函数的参数和调用 | `hooks.cpp`、`wrappers.hpp` |
| `Frame12::submissionDependencies` 与对应执行分支 | 当前所有 Frame12 都来自 Bridge11；从未设置该字段 | `nr/frame.hpp`、`nr/pass.cpp/.hpp` |
| GpuTicket 的跨队列前驱依赖、PrepareSubmission 及两个保留字段 | DX11 桥接始终由自己的同一队列提交，不接入游戏的 DX12 命令列表；当前路径没有这些方法的调用者 | `nr/gpu_ticket.hpp` |
| `timingAllocations`、`timingReuses` | 只累加，没有读取或显示用途 | `nr/pass.cpp/.hpp` |

README 同时修正桥接名称：当前实现是 D3D11/D3D12 共享纹理与共享 fence，不调用 D3D11On12CreateDevice。

**必须保留的内部 D3D12 部分**：模型运行时、PassExecutor12、Frame12、共享纹理、内部队列和命令列表、提交票据、同队列顺序、fence 完成判定、资源预览、曝光计量与生成运动矢量。它们服务于 DX11 游戏的 NR 执行，并非 DX12 游戏捕获残留。CMake 的 d3d12 链接依赖也必须保留。

## 尚未修改的逻辑与性能问题

### 1. P1：窗口消息持有渲染锁调用游戏 WndProc

位置：`src/panel.cpp:344`、`:362`、`:372`、`:454`、`:1343`。

WindowProc 使用 lock_guard 持有 Global().mutex 后调用游戏原始 WndProc；DrawSwapchain 也持有同一锁执行绘制、桥接和模型工作。RouteKeyboard 在判断消息是否相关之前就获得这把锁，因此被消息队列钩子送来的非键盘消息也会等待渲染。

确定的影响是消息处理会受到首次模型/着色器初始化等渲染侧长操作阻塞。如果游戏 WndProc 等待渲染线程，而渲染线程等待此锁，则存在循环等待、卡死风险。这个路径值得优先处理，但不能据此认定已经找到某款游戏丢失焦点的唯一原因。

建议参考 DX12 工程现有处理：只在访问插件窗口/ImGui 状态时持锁，进入原始 WndProc 前释放；解锁后不使用可能已失效的 self/route，窗口销毁清理重新获得锁并重新查找。RouteKeyboard 应先过滤无关消息，再进入插件锁。

### 2. P2：模型初始化失败后重试丢失 NGX 参数块

位置：`src/nr/model_runtime.cpp:89`、`:93`、`:99`、`:101`、`:105`。

Initialize 通过 GetCapabilityParameters 获得参数块后，模型加载、必需导出查询或 Init_Ext 仍可能失败。RetryFailedInitialization 直接把 parameters 清为 nullptr，再次初始化会申请新的参数块；当前工程没有 DestroyParameters 调用。

这是失败初始化后反复关闭/开启 NR 的实际泄漏路径，不能扩大成“正常切换风格每次都泄漏”。源码中“driver-owned, never freed”注释混淆了旧 GetParameters 与 GetCapabilityParameters 的所有权。[NVIDIA 官方头文件](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx.h)规定后者由调用方通过对应 DestroyParameters 释放。

建议记录本插件参数块的所有权，在无 feature、无 GPU 使用的失败重试路径调用对应 D3D12_DestroyParameters；处理销毁失败，不能释放游戏自己持有的参数块，也不能对游戏共用 NGX core 做全局 Shutdown。DX12 工程已有相关所有权处理可参考。

### 3. P2：风格/预设变更连同大型工作纹理一起重建

位置：`src/nr/pass.cpp:151`、`:490`、`:497`、`:571`、`:578`。

SameTuning 不同时，整个 Context 被替换，包含模型 Layer、proxy、answer、output 及转换后的 guides。即使分辨率和格式未变，下次评估仍重新分配工作纹理。

3840×2160 时，仅 proxy 与 answer 两张 RGBA16F 纹理的像素数据就约 126.6 MiB；若 output 为每像素 4 字节，再增加约 31.6 MiB，尚未包括对齐、guides、桥接和模型内部显存。旧 GPU 录制持有旧 Context 时，还可能出现暂时重叠分配。

这是可以减少的切换开销，不是已经证明的永久显存泄漏，也不能用它直接解释 GPU 耗时永久增长。建议分离尺寸/格式决定的工作资源和模型 Layer，在完成必要的 GPU 退役后仅重建 Layer；DX12 工程已经采用类似划分。

### 4. P2：已有完整 NGX 输入时，原生深度追踪仍做多余工作

位置：`src/nr/depth_hooks.cpp:11`、`src/nr/diagnostics.cpp:464`、`:533`、`src/nr/depth11.cpp:207`。

默认 NativeDX11 开启。游戏已经通过 Capture11 提供本帧 NGX 资源时，externallyFed 标记已设置，但 NativeFrame11 仍先调用 depth::EndFrame，再检查该标记并返回。EndFrame 会遍历候选深度并进行 Backup/CopyResource。绘制/绑定钩子的追踪锁开销也仍然存在。

建议在已有本帧显式输入时执行不产生 GPU 备份的统计收尾；不能简单跳过 EndFrame，否则候选年龄、帧计数和统计重置会失真。追踪钩子的省略还要考虑输入源切换和 NGX 偶发缺帧。

### 5. P2：正常 GPU busy 跳帧触发整套光流资源销毁

位置：`src/nr/diagnostics.cpp:278`、`:652`、`src/nr/dense_flow.cpp:67`、`:109`。

估算 MV 路径在 Bridge11 返回 S_FALSE 时调用 DenseFlow12::Reset；Reset 直接丢弃 generation。Bridge11 在槽仍被 GPU 使用时会在录制前返回 S_FALSE，属于正常的非阻塞跳帧。

下一次有效帧 Ensure 重新创建约 42 张纹理和一个噪声上传 buffer。连续繁忙时，这种重复分配会放大 CPU/驱动开销及瞬时显存压力。旧 recording 对 generation 的保留避免了立即释放正在使用的纹理，因此不应把它描述为无条件的提前释放错误。

建议区分时间历史失效与资源存储销毁。忙帧只使历史失效；尺寸/格式变化、错误或最终退役时再销毁资源。复用必须同时维护资源状态及在途 recording 的生命周期。

### 6. P2：首次使用仍在渲染路径编译 HLSL

位置：`src/nr/pass.cpp:223`、`preview.cpp:91`、`white_point.cpp:69`、`bridge.cpp:206`、`dense_flow.cpp:31`。

构建只嵌入部分着色器源字符串；首次模型/预览/曝光/转换/光流使用时会 D3DCompile。相关调用发生在渲染或诊断锁保护的路径，可导致首次操作停顿，并与第 1 条消息锁问题叠加。光流 shader blob 已有静态缓存，不能说每帧都在编译；部分其他对象重建时仍可能重新编译。

建议按 DX12 工程方式在构建阶段编译并嵌入字节码，保持 HLSL 算法不变。PSO 与设备资源仍应在运行时创建。

## 边界与排除项

- **普通 resize 后设备不同导致 NR 永久停止：不能成立。** ModelRuntime 持有 D3D12 设备，而当前桥接使用普通 D3D12CreateDevice。同进程同适配器会复用仍存活的设备，详见 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice)。换适配器/设备丢失恢复是另一问题，不能与普通 resize 混为一谈。
- Bridge11 的 Process 繁忙时直接跳过，没有每帧 CPU fence 等待；GPU 双向同步仍是实际资源安全所需。Drain 的 CPU 等待发生在退役/析构，不是稳态每帧等待。
- Bridge11 析构在 Drain 失败时隔离保留 GPU 状态直到退出，这是失败保护的有意保留；不能直接删除或当成正常运行的无条件泄漏。
- `Layer::~Layer` 仅在最后票据 Complete 时 ReleaseFeature。如果创建成功但随后录制未提交、票据被丢弃，handle 的后续管理可能丢失；属于需补足错误路径验证的释放风险。本轮没有证明正常已提交的风格切换走到此路径。Release 的失败结果也缺少明确追踪。
- 关闭调试视图后，桥接 slot 的 keepAlive 可能保留有限数量的资源直到下次复用或销毁；这是缓存/退役行为，未发现凭此推断的无限增长证据。
- 源码仍有 `LITE_TEST_HOOKS` 条件测试代码，现有构建未定义该宏，没有运行开销和测试导出。它不是 DX12 游戏专用代码，本轮未一并删除。
- 未发现生产代码中的 NVML、NVAPI 或 nvidia-smi GPU 负载/频率轮询。面板 Completed NR GPU 所需的时间戳和读回保留。
- 光流的 Dispatch 辅助函数实际执行 DrawInstanced，不是 compute Dispatch；默认完整路径约 30 次绘制。不能单凭次数给出 GPU 毫秒结论。

## 验证结果与总体评价

清理前完成 Release 全量 clean rebuild（33 步）；清理后通过 Build.ps1 重新编译受影响目标（21 步）并生成 dist/Release。两次均未出现编译警告、语法错误或链接错误。

清理后源码搜索确认上述已删除符号无残留引用；Bridge11 仍设置 orderedQueue、执行实际 Submit，同队列连续评估和 fence 完成检查仍保留。DLL 导出检查仅有 LiteAttachFactory、LiteNrCreate、LiteNrEvaluate、LiteNrRelease、Start，没有测试导出。

构建日志：`build/dx11-review-clean-build-20261012.log`、`build/dx11-cleanup-build-20261012.log`。清理后发布文件 SHA256：

```text
dxgi.dll               362C7C3474CD7F24978FCCEDCF296E3FFE3E665010B60DAE3F8BF10671082EE9
nvngx.dll_litedx11.dll  9ED25670FD5AB480C88B4C42F6902DB89CA9C29579B8A5B583E06858695F2C51
```

框架边界总体合理：代理负责转发/启动，DX11 捕获与呈现独立，内部 D3D12 后端有提交票据和资源保留。主要不足是输入与渲染锁耦合、失败初始化的参数所有权，以及模型生命周期和工作资源生命周期耦合。上述问题仍在，本轮没有声称全部修复。编译通过也不能代替 GPU 同步、游戏兼容性和长期切换稳定性的实际验证。
