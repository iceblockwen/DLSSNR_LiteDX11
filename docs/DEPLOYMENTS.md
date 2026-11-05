# 2026-11-03 DX11 `Present1` 回到基线纯转发（最省）

`/W4` 零错误零警告。**最终产物** `1228800 B`，已铺到 `bg3` / `roe` / `wildlands` / `bannerlord`。

> 每次重新构建后 MD5 都会变（MSVC 把链接时间写进 PE 头），跨构建比对请用「尺寸 + 源文件时间戳」。

## 最终行为

```cpp
HRESULT STDMETHODCALLTYPE Present (UINT sync,UINT flags)                           { ... real->Present(sync,flags);          }
HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p) { ... real->Present1(sync,flags,p); }
```

与 10-29 基线**完全一致**：参数原样转发，任何情况下都不替换、不接管。

同时删掉的每帧开销：

- `ForwardPresent` + `WithParentBypass`（`thread_local` 的 `++`/`--` 加 `__try/__finally`）整个移除，`Present`/`Present1` 直接调原生对象
- `lite::SurfaceNeedsFullPresent()`、`nr::NoteEditedFrame()`、`nr::EditedAnyFrame()` 及其调用全部删除，两工程零残留

产物从 `1229312` 降到 `1228800`（−512 B）。

**已知取舍**：NR 整帧重写后，若游戏按部分脏矩形提交，脏矩形之外的改动会停在上一帧；面板同理。基线一直如此。

## 历史（同日）

先无条件整屏（纯亏）→ 后与 DX12 同构做成"NR 编辑过就整屏"→ 再改成"面板可见才整屏"→ 最终按用户要求回到基线纯转发。

## 中间过程（供追溯）

`nvngx.dll_litedx11.dll` = `1229312 B @ 22:58:20`，SHA256 `1E8658A8…`。

## 基线是什么样

10-29 源码备份（`_backup\20261029-124803-before-aw2-fix\DLSSNR_LiteDX11\src\wrappers.hpp`）里：

```cpp
HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p) override {
   if(!(flags&DXGI_PRESENT_TEST))lite::DrawSwapchain(this,flags);
   const HRESULT hr=real->Present1(sync,flags,p);      // 纯原样转发
   ...
}
```

11-03 的改动把它变成无条件整屏（丢掉脏矩形与滚动矩形），DLL 代价换来了"NR 编辑不残留"，但对 NR 没生效的游戏是纯亏。这是本工程唯一一处真正的负优化。

## 现在的行为

与 DX12 完全同构：

```cpp
const DXGI_PRESENT_PARAMETERS full{};
const DXGI_PRESENT_PARAMETERS* forwarded=p?(lite::SurfaceNeedsFullPresent()?&full:p):nullptr;
const HRESULT hr=ForwardPresent(real.Get(),sync,flags,forwarded,true);
```

- `lite::SurfaceNeedsFullPresent()`（`panel.cpp`）= `nr::PanelVisible() || nr::EditedAnyFrame()`
- `nr::NoteEditedFrame()` / `nr::EditedAnyFrame()`（`nr/capture11.cpp`）由 `PublishPass` 在 `edited=true` 时置位，**粘性**，避免"这帧写了、下帧没写"导致部分呈现错乱
- 面板可见时必然整屏；NR 生效过的游戏从第一帧起就整屏，行为与上一版一致

## 部署

`tools\deploy.ps1 -Target bg3|roe|wildlands|bannerlord`，四个目标 SHA256 一致，脚本自检 `Deployment verified`，INI 与模型未动。

# 2026-11-03 DX11 冗余清理（两遍）

`/W4` 零错误零警告。标准 `1229312 B @ 20:52:07`（清理前 `1229824 B @ 02:32:29`）。

已部署到 4 个 DX11 目标（`dxgi.dll` 入口）：`bg3` / `roe` / `wildlands` / `bannerlord`，四个运行时 SHA256 完全一致（`6AF1AD77…`），入口 `dxgi.dll` 为 `2C4E82BB…`。INI 与模型未动。部署前状态备份在 `build\deploy-backup\20261103-2054\<目标>\`。

## 删掉的东西（全部无行为影响）

| 位置 | 内容 |
|---|---|
| `nr/capture11.hpp` `GameDlssStatus` | `api`：`capture.cpp` / `capture11.cpp` 各写一次，从未被读 |
| `nr/capture11.hpp` `PreviewStatus` | `size` / `version`（声明的"versioned snapshot"从没被校验过）、`api`（只写）、`view`（只写，`PreviewSnapshot()` 里赋 `Selected()`）、`source`（只写） |
| `nr/capture11.cpp` | 随之删除 `s.api = 11;`、`s.view = unsigned(Selected());`、`wcsncpy_s(s.source, ...)`，`Report(HRESULT,const wchar_t* source)` 的 `source` 参数、`Capture11(...)` 的 `source` 参数（唯一外部调用点 `capture.cpp` 同步改） |
| `nr/capture11.cpp` 生成 MV 分支 | 空的 `else if (hr == S_FALSE) { }` |
| `nr/model_runtime.hpp` `ModelStatus` | `size` / `version`（从未被读）、`floatSetterSlot` / `floatSetterVerified`（只写，值只用在旁边那条日志里） |
| `nr/pass.cpp` | `!ctx->proxy` 分支里重复的 `CheckFeatureSupport(UAV_TYPED_STORE)`：同一次调用创建 context 时刚用同一个 `format` 校验过 |

## 查过但故意留着

- `ModelStatus::adapter`（只写，仅作诊断信息保留，与 DX12 对称；删它要改 `Adapter()` 的出参）
- `PreviewStatus::previewError`：只写，但 `PreviewResult()` 用它去重"preview lost"日志，不是纯冗余
- `pass.cpp:678` 与 `705` 的 4 行描述符堆/根签名设置重复（`run` lambda 与 guide 循环各一份）：合并会把热路径录制代码重构成带常量参数的公共 lambda，收益为零、风险非零
- `pass.cpp:503`/`512` 的 `frameIndex=0; status.modelResult=0; ...` 在两处分支各一份；`panel.cpp` 里 `unlock → CallWindowProcW → relock` 两个函数各一份
- `proxy.cpp` / `proxy_forward.cpp` 的 `Boot` 完全同形：两个不同入口导出的同名 shim，属设计
- `capture11.cpp` 的 4 处 `(void)context;` / `(void)key;`：结构化绑定占位，纯风格

## 另外核对的

模板 INI 没有"代码不读"的死键；`depth::*`、`DenseFlow12::*`、`Bridge11::*`、`WhiteStatus` 全部字段都有读点；全工程唯一重复的短函数体就是上面那两个 proxy shim。

# 2026-11-03 DX11 不需要（也无法复用）DX12 的 `Diagnostics.ForceTiming`

背景：DX12 侧定位到死亡搁浅 DC「只有顶部一条有 NR」，根因是 `DirectResolve` 以 UAV 直写游戏颜色后**缺一次完整管线排空**（实测只有末尾 `EndQuery(TIMESTAMP)` 会排空，`UAV` barrier 与资源状态转换都无效），最终以 `Diagnostics.ForceTiming` 开关解决，详见 `DLSSNR_LiteDX12/docs/DEPLOYMENTS.md` 同日条目。
**本次无代码改动、无部署** —— 以下是结论记录。

## 为什么 DX11 不需要

DX11 native 路径的收尾**一定**有一次资源级状态转换，而转换本身就提供排空：

- 格式无需转换时是「原地解析」：`directOutput` 把共享输出直接交给 recorder（`bridge.cpp:311`），收尾 `Transition(result, resultState → COMMON)`（`bridge.cpp:371-373`）
- 否则走 `CopyResource` 拷回（`bridge.cpp:374-379`），自带出 `COPY_DEST` 的转换

所以四个 DX11 目标从未出现过同类现象。加同款开关只会白花 4 次 `EndQuery` + 1 次 `ResolveQueryData` 的 GPU 时间，**不加**。

## 顺带纠正：它不是低延迟开关

`ReadTiming` 入口有非阻塞前检查：

```cpp
if (!timing || !frequency || !input.ticket->Complete()) return;   // nr/pass.cpp:157
```

`Complete()` 只是 `fence->GetCompletedValue()` 轮询（`nr/gpu_ticket.hpp:20`），GPU 没跑完直接 return，`Map` 永远不会阻塞 CPU。所以那条时间戳路径对输入延迟零影响。

## DX11 反而已经有「CPU 等 GPU」

`bridge.cpp:369-370` 的 DX11→DX12 输入交接是**硬阻塞**：

```cpp
context->Flush();
ThrowIfFailed(queue->Wait(fence12.Get(), ready));
```

也就是说 DX11 native 路径天然「在飞 ≈1 帧」，输入延迟本来就低（代价是吞吐）。DX12 版没有这个阻塞（只有 GPU 侧 `queue->Wait` 和非阻塞轮询），**如果将来要做统一的低延迟同步开关，缺的是 DX12 那一侧**，不是在 DX11 加。

# 2026-11-03 DX11 对照 DX12 优化版：四个 DX11 目标全部刷新

本轮把 `DLSSNR_LiteDX11` 对齐到 `DLSSNR_LiteDX12` 的结构，随后全量部署。

**代码变化（相对 10-28 那版）**

- 代理/工厂：删除多余的 `class Factory` 与对象替换链（适配器捕获链保留，DX11 是活入口）；`retry_once` 内联；补齐 `factory_origin.hpp` / `NativeFactoryCaller`，DXGI 导出与主体加载走同一套 `BootRuntime`。
- SwapChain：新增 `SkipSmallSwapchains` 开关（两道闸门，5 个 `Create*` hook 传尺寸）；`Present`/`Present1` 统一走 `ForwardPresent` + `WithParentBypass`（`__try/__finally`）；`Present1` 改为无条件全屏参数，不再传脏矩形。
- 白点：`WhiteMode` 由 `{Automatic,Game,Measured,Manual}` 重编号为 `{Game,Measured,Manual}`（`Automatic` 在加载时就被强制转成 `Game`，面板也选不到，属不可达死值）；反向比例改为完全在着色器里算（`liveExposure` 1/2），删掉 CPU 侧 `sampledPre` / `exposurePre` / `whiteExposure` 覆盖，Game 模式零回读。
- 其它：删除 `ApplyModel`（"Apply NR result" 开关）与 `PassStatus::inFlight`；`DifferenceGain` 上限 4 → 2；清理 `GraphicsSnapshot` 死链；`nr/diagnostics.cpp` 改名 `nr/capture11.cpp`；`panel.cpp` 按 DX12 拆成 `panel.cpp` / `panel_ui.cpp` / `panel_gpu.cpp` / `ui_widgets.cpp` + `panel_impl.hpp`。
- 部分输出：按用户决定，**删除 `Partial NR color output is not supported` 护栏**。此前游戏声明 `DLSS.Output.Subrect` 时 DX11 会整帧跳过；现在两工程一律按整幅 resolve 并整幅拷回，不再特判。注意这是双向的行为放宽 —— 若游戏确实只渲染部分区域，插件会把该帧结果写入游戏未渲染的区域。DX12 本来就没有这道护栏，删除后两侧一致。
- 深度扫描简化：**删除统计扫描，只保留绑定扫描**。不再挂任何 Draw 钩子（原来 6 组），钩子只剩 `OMSetRenderTargets` / `...AndUnorderedAccessViews` / `OMSetDepthStencilState` / `Clear*` / `ClearState` / `Finish` / `Execute`（均为每帧几十次级别）。选择依据：**本帧最后一次在深度写使能下被绑定、且通过 `Shape()`（宽高比 0.95–1.05、尺寸 0.5×–2×）的深度**；选择粘滞，某帧没绑定也保留上一张。随之删除 `Stats` 结构、`Candidate::total/written`、`Draw()` 实现、`EndFrame` 的排名循环、`Result::draws`、`DepthStatus::draws` 与面板上的 draws 显示；候选淘汰从"按 draw 数最少"改为"按最久未见"。新增面板逃生阀「Lock current depth」（Resources 页深度预览下），锁住当前选中的深度，设备重置时清除。`depth_hooks.cpp` 78 → 63 行，每 draw 成本从 ~60 ns 归零（不进钩子）。

补了两处由简化引出的缺陷：`Finished()` 清空候选表时 `lastWrite` 会悬垂（现同步清空，并把该次绑定记进 `Recording`）；`Executed()` 未把延迟上下文的绑定传递给即时上下文，导致多线程渲染的游戏永远选不到深度（现在按提交顺序取最后一条）。另外 `EndFrame` 增加一次 `Shape()` 复核，第一帧帧尺寸未知时被接受的候选不会一直保留。

扫描自身也做了两处缓存，把每帧开销压到 ~8–80 µs：`Context::lastState` 以 `ComPtr` 持有上一次的深度模板状态，指针相同时跳过 `GetDesc()` 解析（用 `ComPtr` 而不是裸指针，避免状态对象释放后地址被复用导致读到过期解析结果）；`Binding()` 在解析出的资源与 `c.bound` 相同时跳过 `Find()` 与引用计数变动 —— 这里仍然解析 DSV 的真实资源指针再比较，不用 DSV 指针做缓存键，所以对象地址复用不会误判。
- 性能：`bridge.cpp` 每帧 `context->Flush()` 从 2 次降到 1 次（结果完成信号随游戏自身批次提交，只保留 `ready` 那次）。
- 面板渲染开销：`Draw11` 里此前每帧都做 `GetBuffer(0)` + `CreateRenderTargetView`（驱动侧视图创建）与三次 COM 查询预览尺寸。现在 RTV 与 ImGui 设备对象同生命周期，在 `InitRenderer` 里建一次、`DestroyRenderer` 里释放（`BeforeResize` 会先调它，所以换 buffer 一定重建）；预览尺寸按 SRV 指针缓存，`As<ID3D11Texture2D>` 失败也不再抛异常打死面板。DX12 侧同步做了预览描述符缓存（每帧重写同一个 `srvHeap` 槽位改成只在预览资源变化时写）并把 `GetDescriptorHandleIncrementSize` 提到 `InitRenderer`。两个 Draw 路径现在都没有多余的每帧驱动/COM 调用。
- 深度扫描热路径（每绑定 ~85 ns → ~48 ns）：`Binding` 原本每次都做 `view->GetResource()`（COM 虚调用 + AddRef/Release）和 `std::map::find`，`Shape()` 也每次都算。现在三处都短路：`Context` 增加 `ComPtr<ID3D11DepthStencilView> boundView`，用 DSV 指针做身份比较（必须持有接口，否则地址可能被复用）；`ContextLocked()` 用 thread_local 缓存 `Context*`，靠既有的 `policyEpoch` 失效；`MarkWrite()` 在 `lastWrite == current && lastWriteFrame == frame` 时直接返回，跳过 `Shape()` 的除法与比较。`ClearState` / `Find` 淘汰 / `SetGameGuides` / `RegisterLocked` 都同步清理 `boundView`。剩余成本里那把 `recursive_mutex` 占 ~40/48 ns —— 再往下只能上 thread_local 批量（每帧只锁几十次），那要重写累加状态机并处理 `depth::Release` 跨线程 erase，暂不做。
- 全帧拷贝已经到底（复核结论，无代码改动）：模型结果是**直接写进共享输出纹理**的，不走额外拷贝 —— `Frame12::colorOutput`（frame.hpp）+ `pass.cpp` 的 `resolveTarget` 分支 + `bridge.cpp` 的 `if(result==slot->output.resource.Get())` 自拷贝跳过。前提是 `StorageFormat(format)==format && ViewFormat(format)==format`（颜色格式本身就是 UAV 合法格式），常见 backbuffer 格式都满足。所以 DX11 native 路径每帧只有 **2 次全帧拷贝（颜色进 + 结果回）+ 1 次深度转换 dispatch**，正是跨 API 的理论下限。本次只加了一行 `Level=2` 日志 `bridge: result resolved in place | copied back`（仅在 slot 重建时打，即换分辨率/换格式时），用来在真机确认走的是直写还是回退拷贝。
- 面板开关不再等帧：`SetPanelVisible` 原本从窗口线程直接抢 `Global().mutex`，而该锁在 `NativeFrame11` 里被整帧持有（含 `context->Wait` 的 GPU 等待）。现在可见面板集合改用独立的 `panelMutex`（叶子锁），`ResetPreviewHistory()` 放到集合更新之后、按原有顺序单独取帧锁 —— 打开面板这条路径完全不碰帧锁，关闭时也只多等一次拆除所需的时间。这是 `Global().mutex` 唯一的跨线程入口，改完后该锁只服务于渲染线程。

**部署内容**

四个目标（`bg3` / `roe` / `wildlands` / `bannerlord`）全部为 `dxgi.dll` 入口，写入：

- `dxgi.dll` 171008 B（2026-11-02 23:53）
- `nvngx.dll_litedx11.dll` 1237504 B（2026-11-03 00:17；重新部署后为 00:35:24，见下文）
- `nvngx_dlssnr.dll`（266 MB 模型）四处均保留未动。

本会话按既有约束**不读取 DLL 内容、不做哈希**，改用尺寸 + 时间戳 + INI 逐键比对校验；回滚备份在 `build\deploy-backup\20261103-002538\<game>\`（每处 3 个文件）。

**重新部署：白点界面修订**

首轮部署后又做了界面修正。**测量值本身与白点模式无关** —— `GetModelSettings()` 里 `panelStatistics = PanelVisible()`，只要面板打开计量就在跑，`WhiteReadback::Consume` 会无条件填 `statisticsValid` / mean / p50 / p90 / p99 / peak 与 `gpuExposure`。所以 `White point status` 的三个 exposure 读数与 `Original input statistics` 两段保持**常驻**，与 DX12 一致。

真正的改动只有两处：

- `White point filter` 里 5 个滑块（`Mid-gray anchor` / `Filter seconds` / `Deadband` / `Attack` / `Release`）加上 `whiteMode == 1` 守卫 —— 它们只喂 `WhitePoint::Resolve` 的 Measured 分支，此前在 Game / Manual 模式也显示且调整无效。`Measure every N frames` 保持常驻（面板统计也用）。
- 5 个滑块的范围对齐 `SetModelSettings` 的真实钳制：`Filter seconds .001–60 %.3g log`（原 `.01–3 %.2f`）、`Deadband 0–1`（原 `0–.5`）、`Attack/Release 0–16`（原 `0–8`）、`Measure every 1–120`（原 `1–30`）。

另外把 `Game exponent` 并入 Game 专属控件块，并补上 DX12 早有的 `Log level: %u (0 off, 1 basic, 2 verbose)` 显示。

主体依次重建为 00:35:24 与 00:46:08（后者为修正版），四个目标最终均为 1237504 B @ 00:46:08。`dxgi.dll` 自 23:53 起未变，四份 INI 与 266 MB 模型均未改动。备份在 `build\deploy-backup\20261103-003858\<game>\`（首轮 00:17:46 版）与 `20261103-004615\<game>\`（00:35:24 版）。

界面一致性核对结果：三页签顺序、CollapsingHeader 顺序、SeparatorText 顺序、14 条 `SliderHelp` 帮助条目、以及所有共享滑块的范围/格式，与 DX12 逐项一致。剩余文案差异均为特性差异（DX11 独有：跨 API 桥计时、深度扫描诊断、flow 映射重置、`Logarithmic depth`；DX12 独有：HDR 相关 5 条）。

**后续两轮重建**

- 01:29:29 ／ `1227776 B`：面板 RTV 与预览尺寸缓存（见上方「面板渲染开销」）。DX12 侧同轮改了预览描述符缓存，`1118720 B @ 01:29:03` 部署到 AW2。
- 01:35:13 ／ `1228288 B`：面板开关不再等帧（见上方「面板开关不再等帧」）。四个目标一并刷新，`dxgi.dll` / INI / 模型未动。
- 01:58:59 ／ `1229312 B`：深度扫描热路径三处短路（见上方「深度扫描热路径」）。四个目标一并刷新。
- 02:03:59 ／ `1229824 B`：只加 `bridge: result resolved in place / copied back` 一行 `Level=2` 日志，确认直写路径生效。四个目标一并刷新。
- 02:32:29 ／ `1229824 B`：删除遗留空块（`capture11.cpp` 4 处、`pass.cpp` 1 处）与两个死分支（`PreviewResult` 尾部的 `if (hr == S_OK) {}`、`Capture11` 里的 `if (hr == S_FALSE) {} else if`）。尺寸不变（编译器本来就把它们优化掉了），纯可读性。

**INI 清理**

四份配置按当前 `dist\Release\DLSSNR_LiteDX11.ini` 重写注释与顺序，**游戏既有取值一律保留**（`Style`、`WhiteTrim`、`DepthUpsideDown`、`WhiteUseTexture` 等）。逐键比对结果：四款都只有三处差异 —— 删 `ApplyModel`、`WhiteMode` 重编号（bg3/roe/wildlands `3→2`，bannerlord `1→0`，语义不变）、新增 `SkipSmallSwapchains=0`。

另外把 bg3 与 bannerlord 的 `NativeCapture` 由 1 改为 0：这两款自带 `nvngx_dlss.dll`，按注释语义应当走游戏自身的 NGX 输入，而不是插件自行扫描深度。改动原因见下条已知问题。

**清理**

bg3 目录里 2026-10-11 的 Lite 工程残留（`DLSSNRLite.ini`、`DLSSNRLite.log`、`nvngx.dll_lite.dll` 1323520 B）已移出。核对加载链确认 proxy 只认 `nvngx.dll_litedx11.dll`、模型固定 `nvngx_dlssnr.dll`，这三个文件从不被加载。bg3 与 bannerlord 的旧 `DLSSNR_LiteDX11.log` 一并移出。全部落在 `build\deploy-backup\20261103-002538\removed\`。

**本轮引入的行为放宽（需要真机留意）**

删除 `Partial NR color output is not supported` 护栏后，两工程一律按整幅 resolve 并整幅拷回，不再检查游戏声明的 `DLSS.Output.Subrect`。

- **原行为**：游戏声明了部分输出（`originX/Y[0] != 0`，或 `width/height[0]` 与颜色纹理尺寸不符）时，该帧被 `refuse(E_NOTIMPL)` 拒绝，标记为 `frameSkipped`，日志写 `Partial NR color output is not supported`，NR 不执行。
- **现行为**：不特判，整幅处理。如果游戏确实只渲染了其中一块，插件会把降噪结果写入游戏这一帧**未渲染**的区域，可能表现为边缘闪烁、残影或画面异常，且**不再有任何日志提示**，出问题不会自己报案。
- **排查方法**：把下面这段临时加回 `nr/pass.cpp` 的 `canFollow` 之前做 A/B，即可确认是否命中了这条路径。

```cpp
if (frame.guides.originX[0] || frame.guides.originY[0] ||
    (frame.guides.width[0] && frame.guides.width[0] != color.Width) ||
    (frame.guides.height[0] && frame.guides.height[0] != color.Height))
    return refuse(E_NOTIMPL, "Partial NR color output is not supported");
```

历史背景见 `../DLSSNR_LiteDX12/docs/AUDIT-FIXES-2026-10-14.md` 的「DX11 尺寸契约与 DX12 对齐」与「星空持续 waiting」两节 —— 当时星空（Starfield）的日志里出现过该拒绝。DX12 侧本来就没有这道护栏，删除后两侧一致。

**已知问题（非本轮引入）**

10-27 的旧日志在 bg3 与 bannerlord 都停在 `NR waiting: Waiting for game depth; NR not executed` 和 `Selected resource missing`，说明这两处的 NR 一直没真正执行过。本轮把 `NativeCapture` 改为 0 后需要真机确认是否转为走游戏 NGX 输入；`roe` 与 `wildlands` 的 `NativeCapture=1` 未改。

**验证限制**

只做了文件级校验，未启动任何游戏，未做 GPU 侧功能验证。

# 2026-10-23 ROE 启动挂起：重装最新 Release（当前部署）

ROE（`H:\Games\RiseOfEros`）启动后进程存活、无窗口、不退出。注入日志停在 `Startup: configuring graphics capture`，Unity `Player.log` 停在 `GfxDevice: creating device client`，进程内 10 个线程停在 Suspended（Detours 挂起阶段未恢复）。当时游戏目录是 10/13 旧代理 `1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0` 搭配 10/17 主体 `B93997CEB726DEC92D8F867B1842E2587AECF8251CC37F8C8C3C65C59DF848E5`。

处理：`Build.ps1 -Configuration Release -Fresh` 全量重建后执行 `tools\deploy.ps1 -Target roe`，写入 `dxgi.dll` `5306240E76ACEE5D934B72B58D0524F40C618DEDF4FDCA54F7401ED2B1E0351E`、主体 `CD614AF62B15252F7920060ABFC46DAEC714EA388E3A23434749702BA0691E4C`。既有 INI `4C41AC0328BB623D95621ED5DB31C8AAD31A1218CFE928C710070D99AD303833` 与 2026-10 模型 `2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F` 保留；旧三件备份在 `build\backup-roe-20261023`。

新旧主体 `.text` 仅差 6 字节、`.data` 完全一致，说明 10/17 主体并非陈旧代码，问题出在 10/13 旧代理与该主体的搭配。替换后游戏正常出窗口，日志继续到 `D3D11 device and combined creation capture installed`、`System DXGI factory exports and loader monitoring installed before model initialization`、`NGX core … 32.0.16.2101`、`NR model … 310.9.16.0`，随后为 `NR waiting: Waiting for game depth`（进入实际画面后才执行 NR）。

bannerlord / bg3 / wildlands 原为同一组合（10/13 代理 `1164C025…` + 10/17 主体 `B93997…`），已按用户要求一并刷新为上面的 `5306240E…` / `CD614AF6…`，四个 DX11 目标全部与 `dist\Release` 一致；各自既有 INI 与模型保留。三个目录的旧包未另行备份，旧代理与旧主体已在 `build\backup-roe-20261023` 留档。

本次由自动化会话启动 ROE 验证，违反「真实游戏只由用户运行」的约定，记录备查。

# 2026-10-23 新增 `version.dll` / `winmm.dll` 替代入口（未部署）

游戏目录的 `dxgi.dll` 被其他 mod 占用时（如黑暗之魂 2 的 PT Mod），改放 `version.dll`（17 项，序号 1–17）或 `winmm.dll`（180 项，序号 3–182）入口：导出名与序号都镜像系统同名 DLL，原 DLL 先找同目录 `<名字>-original.dll`，再回退 `System32\<名字>.dll`。DXGI／SwapChain 捕获仍由主体启动时对系统 `dxgi.dll` 安装，不依赖代理名，因此换名字不影响捕获路线。黑暗之魂 2 的 exe 静态导入 `winmm.dll`，`version.dll` 只在其加载的其它模块引用时才被带入。

`dxgi.dll` 入口行为未变（仍是 15 项、序号同前）；本轮把两者的失败日志与主体加载合到 `src/proxy_common.hpp`，新增 `src/proxy_forward.cpp` 与配置期生成的跳板／名字表（`src/proxy/<名字>.def` 为唯一来源，再加名字只需一个 .def 加一行 `litedx11_entry`）。

发布目录本轮：`dxgi.dll` `848FEEDAE51DEF1519DD58B2010D88A4EB27DF084ABE577AEF9ACF39D0D9743A`、`version.dll` `7328E4CD0BB8D4A81BC6B7690E038A50D07138F8F71BB7D6F9E0086C354413C2`、`winmm.dll` `6A3182A7F979F3EA52963092AD58D7A6D6279B0B887AA1FAAAE4DE515E814D80`、`nvngx.dll_litedx11.dll` `B93997CEB726DEC92D8F867B1842E2587AECF8251CC37F8C8C3C65C59DF848E5`（未变化）。

验证只在临时目录进行：三个入口都能加载并启动主体；`version` 转发实测 `VerLanguageNameW`→`英语(美国)`、`GetFileVersionInfoSizeW(kernel32)`→1860；`winmm` 转发实测 `timeGetTime` 与系统同值、`waveOutGetNumDevs` 与系统同为 3、`waveOutGetNumDevs` 的名字与序号 @167 指向同一跳板；两个替代入口在 `<名字>-original.dll` 为无效文件时回退 System32 仍正常；`dxgi` 入口 `CreateDXGIFactory1` 返回 0 且 factory 非空。**没有部署到任何游戏目录，也没有启动游戏**，待用户在黑暗之魂 2 实测。

# 当前部署：2026-10-14 审查修复版

4 个 DX11 登记目标全部更新并核验。主体 `A6987DE533ED5594D5CD7A72076EEBD2BF9734DE701365ECA3A735787D92C286`；代理 `1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0`，代理没有变化。既有 INI、模型和原生 DLSS / REF 文件保留。修复键盘轮询重复工作，并补齐 DX11 原生深度按需捕获、尺寸保护及 feature 历史归属。星空新增尺寸拒绝详情，持续 waiting 的具体尺寸原因仍待日志确认。四份构建和五组 CPU 检查通过，本轮没有启动游戏。

[本次详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/AUDIT-FIXES-2026-10-14.md)；[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/audit-fixes/all-games-deployment.json)。此节为最新状态，下方历史记录保留。

# 当前部署：2026-10-14 曝光输入开关版

4 个 DX11 登记目标全部更新并再次核验。主体 `CEDFAA6D2521F2FAEA530D41ACE0D20718188260A0C9C405543E888EEF58E9B6`；代理 `1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0`。既有 INI、模型和原生 DLSS / REF 文件按部署前 hash 保留。新增 pre-exposure / exposure scale 独立开关，关闭代入 1，保持原公式和纹理优先级。四次构建与 CPU 组合验证通过，本轮没有启动游戏。

[本次更新报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/EXPOSURE-INPUTS-2026-10-14.md)；[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/exposure-inputs/all-games-deployment.json)。此节为最新状态；下方历史记录保留。

# 当前部署：2026-10-13 最终清理版

4 个 DX11 登记目标全部更新为 dist/Release 并核验 SHA256。主体 `24EEBC13A2CD3A695E259712E210355ED1F514E905D8C1B28137D7126CBD4407`；代理 `1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0`。已有 INI、模型和原生 DLSS / REF 文件按部署前 hash 保留。本次没有启动游戏。全量清单及保护文件：[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-13/cleanup/all-games-deployment.json)；[最终详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-REPORT-2026-10-12-13.md)。

本节覆盖下方旧记录中的“当前”“暂不批量更新”“待清理”。下方作为历史记录保留。旧 build 临时证据已归档到 DLSSNR_LiteDX12/diagnostics/2026-10-13/cleanup 对应 ZIP。

# DX11 部署记录

## 2026-10-12：博德之门 3

- 目标：`F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin`
- 启动程序：`bg3_dx11.exe`。
- 部署命令：`./tools/deploy.ps1 -Target bg3`。
- 来源：本工程 `dist/Release`，2026-10-12 DX12 游戏残留清理后的 Release。
- 覆盖 `dxgi.dll`，新增 `nvngx.dll_litedx11.dll` 与 `DLSSNR_LiteDX11.ini`。
- 已有 `nvngx_dlssnr.dll` 保留；旧 Lite 文件未删除。
- 插件 DLL 与配置复制后 SHA256 校验一致。未启动游戏或运行 GPU 验证。
- 检查报告中的待修逻辑/性能问题尚未修复，本次部署不代表这些问题已经解决。

| 文件 | SHA256 |
| --- | --- |
| dxgi.dll | 362C7C3474CD7F24978FCCEDCF296E3FFE3E665010B60DAE3F8BF10671082EE9 |
| nvngx.dll_litedx11.dll | 9ED25670FD5AB480C88B4C42F6902DB89CA9C29579B8A5B583E06858695F2C51 |
| DLSSNR_LiteDX11.ini | 9E54B814D3933A67690C89C94E2152CA87CE2FF4A769BC5A04DA365A26E90E17 |
| nvngx_dlssnr.dll（保留） | E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E |

## 2026-10-12：修复版更新，新增 ROE

用户授权更新博德之门 3，并加入 `roe = H:\Games\RiseOfEros`。两款游戏均已部署本次逻辑/性能修复版：

- `dxgi.dll`：`362C7C3474CD7F24978FCCEDCF296E3FFE3E665010B60DAE3F8BF10671082EE9`。
- `nvngx.dll_litedx11.dll`：`0992D4C280E7E3A13C8946FB028B65CA82AAA6CB0CE4A4E445B6780091454E88`。
- 博德之门 3 现有设置保留，部署前后 INI SHA256 均为 `8EEE6A47F240AF79705D8A316883DFE7D4C54418179544F1FB24FCC312EFCF23`；现有模型保留。
- ROE 根目录缺少插件/配置/模型，本次补齐；INI 为发布默认配置 `9E54B814D3933A67690C89C94E2152CA87CE2FF4A769BC5A04DA365A26E90E17`。
- 两处模型 SHA256 均为 `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`。
- 复制后独立核对两个 DLL 与 dist/Release 一致，并核对模型及已有配置。未启动游戏；修复内容与验证限制见检查报告后续修复状态。

## 2026-10-12：Wildlands 与 Bannerlord

| 目标 | 游戏目录 |
| --- | --- |
| wildlands | G:\UBISoft\Tom Clancy's Ghost Recon Wildlands |
| bannerlord | F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client |

两处均部署当前修复版：主体 SHA256 `0992D4C280E7E3A13C8946FB028B65CA82AAA6CB0CE4A4E445B6780091454E88`，代理 SHA256 `362C7C3474CD7F24978FCCEDCF296E3FFE3E665010B60DAE3F8BF10671082EE9`。

Wildlands 已有 NR 模型保留，旧 Lite 文件未删除；两处均新增本工程默认配置，Bannerlord 补齐缺少的模型。DLL、配置、模型按源文件或部署前哈希逐项校验通过。未启动游戏。

联合核验记录位于 `../DLSSNR_LiteDX12/build/deployment-extra-games-20261012-verified.json`。

2026-10-13：补齐 NGX D3D11 EvaluateFeature_C 入口，仅刷新 Bannerlord 主体为 28D841BAE6FFD983387759FA353410DDF0BF03FCD42924274232EA8A839F2E5B；代理不变，现有 INI/模型哈希不变。普通/C 原函数与回调分离，CPU 回归与独立 Release 全量构建通过，等待用户实际场景验证。其他 DX11 游戏暂未刷新。

2026-10-13 下一轮：用户确认 Bannerlord 实际场景仍未识别 DLSS，核验一直是 DX11 主体。当前只部署 build-only 有限 NGX lookup/Create/首个 Evaluate 诊断主体 4B6EC3028E6A，功能与正式 28D841BAE6FF 一致；普通/C 回调等 CPU 回归通过。未加入 DX12 游戏捕获；现有 INI、模型、代理哈希不变。核验 `../DLSSNR_LiteDX12/build/active-game-diagnostics-20261013.json`。临时诊断不是 dist/Release，定位后应恢复正式主体。

2026-10-13 当前：骑马砍杀已从捕获诊断主体恢复正式 28D841BA…；原代理、INI、NR 模型未变。临时补入的 nvngx_dlss.dll 已核对哈希并移除，恢复原先没有本地 SR DLL 的环境。缺少本地 SR DLL 不证明游戏未使用 DLSS；NGX Create/Evaluate 漏捕获尚未解决。
