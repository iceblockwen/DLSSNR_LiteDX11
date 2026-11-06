> **2026-10-25 备注：** 第 168-172 行关于 `depth::EndFrame` / `Binding` / `BeforeBind` 反复 `Backup` 的建议已实施，实现方式改为"零拷贝、直接使用活纹理"；`Backup` / `BeforeBind` / `BeforeFinish` 已不存在，`PSPixels` / `ControlMask` 亦已删除。以当前源码为准。

# DLSSNR_LiteDX11 全工程梳理与问题报告

**日期：** 2026-10-25
**范围：** `src/` 全部 C++/HLSL（约 5,000 行）、`CMakeLists.txt`、`Build.ps1`、`DLSSNR_LiteDX11.ini`、`tools/`、`assets/`
**方法：** 逐文件静态通读 + 符号引用计数 + 以 MSVC `/W4 /std:c++20` 单独编译 18 个 `.cpp`（见第 9 节）
**限制：** 没有启动游戏、没有 GPU 抓帧、没有基准测试。下文性能结论是代码级推断，不是实测数字。

> **更新（2026-10-25，第二轮）：** 第 5 节的 DC-1 ~ DC-11 已清理完毕，并通过独立目录的全量 Release 构建（39 步，0 警告 0 错误，4 个 DLL 全部链接成功）。逐条记录见第 10 节。

---

## 1. 工程结构梳理

### 1.1 产物与职责

| 产物 | 源文件 | 职责 |
| --- | --- | --- |
| `dxgi.dll` | `proxy.cpp`、`dxgi.asm`、`dxgi.def` | 只做系统 DXGI 15 项导出转发 + 启动主体；不持有图形/UI/模型对象 |
| `version.dll`、`winmm.dll` | `proxy_forward.cpp` + 生成 .asm/.hpp | 同名系统 DLL 的导出镜像（17 / 180 项），启动方式与 dxgi 一致 |
| `nvngx.dll_litedx11.dll` | `litedx11_runtime` + `litedx11_frames` | 插件主体：Hook、捕获、面板、桥接、NGX 调用 |
| `litedx11_frames.lib` | `bridge.cpp`、`pass.cpp`、`dense_flow.cpp`、`depth11.cpp`、`white_point.cpp`、`preview.cpp` | 与运行时解耦的帧处理/GPU 后端 |

### 1.2 运行时子系统

| 子系统 | 位置 | 说明 |
| --- | --- | --- |
| 启动 & 日志 | `bootstrap.cpp`、`runtime.cpp` | `Start()` 一次 `call_once`；日志经队列线程追加写文件 |
| Detour 事务 | `detour_hook.hpp` | 每个签名一个 8 槽表 + 单次事务提交，失败整体回滚 |
| DXGI/D3D11 创建 Hook | `hooks.cpp` | 工厂导出、工厂/交换链包装、`D3D11CreateDevice*`、`LoadLibrary*`、`GetProcAddress` |
| 交换链包装 | `wrappers.hpp`、`panel.cpp` | `Present`/`Present1` → `DrawSwapchain`；`ResizeBuffers*` → 面板与深度状态重建 |
| 面板 & 输入 | `panel.cpp`、`input*.cpp` | ImGui DX11 后端、窗口子类、消息队列/原始输入/DirectInput/Cursor Hook |
| NGX SR 捕获 | `capture.cpp` | `CreateFeature`/`EvaluateFeature`/`EvaluateFeature_C`/`ReleaseFeature` 8 槽 thunk |
| 原生深度捕获 | `depth11.cpp`、`depth_hooks.cpp` | Draw/Clear/OMSetRenderTargets 追踪 + 候选打分 + 备份纹理 |
| D3D11↔D3D12 桥接 | `bridge.cpp` | 共享纹理 + 双槽 + 共享 fence，每上下文一个 transport |
| NR 执行 | `pass.cpp` | 模型创建/评估、HDR 编码/合成/色调保护、时序与白点 |
| 光流估计 | `dense_flow.cpp` + `dense_flow.hlsl` | 8 级金字塔 + 匹配/池化/上采样，生成 MV + 信任掩码 |
| 模型运行时 | `model_runtime.cpp`、`ngx_parameters.hpp` | NGX core/model 发现、能力块所有权、参数写入槽探测 |
| 配置 | `config_storage.cpp`、`config_writer.cpp`、`model_settings.cpp` | 三级回退（插件目录 → 用户 profile → 显式目录）、后台合并写 |

### 1.3 帧数据流（关键路径）

```
游戏 DX11 帧
├─ NGX SR 路径: EvaluateFeature → capture.cpp thunk → Capture11()
│                                     └─ diagnostics::Model11(estimate=false)
└─ 原生路径  : Present → Panel::Draw → diagnostics::NativeFrame11()
                                      ├─ depth::EndFrame() 取候选深度备份
                                      └─ diagnostics::Model11(estimate=true) → DenseFlow12
Model11 → Bridge11::Process
      → D3D11 复制/转换 3 张输入 → Signal+fence → DX12 队列 Wait
      → PassExecutor12::Record（编码 → NGX Evaluate → 合成 → 白点计量 → 复制到共享输出）
      → DX12 Signal → D3D11 Wait → 共享输出 SRV
      → D3D11 CopyResource 回游戏颜色纹理
```

默认状态：`NeuralRendering.Enabled=0`、`ResourceView=0`，因此 `ProcessingActive()` 为假，深度/Draw Hook 与桥接都不工作；常驻的只有交换链包装、面板（隐藏时仅 `Present` 回调）和输入 Hook。

---

## 2. 结论摘要

| 类别 | 条目数 | 最高等级 | 说明 |
| --- | --- | --- | --- |
| 性能 | 12 | 高 | 面板锁串行化、每帧多次全屏复制、深度候选备份与 O(n²) 扫描 |
| 逻辑/正确性 | 11 | 高 | 未提交录制会拆掉整个模型上下文；设备变更后 NR 永久失效 |
| 死代码/遗留 | 14 | 中 | 11 项已清理，3 项按理由保留（见第 5、10 节） |
| 日志 | 6 | 中 | 状态抖动刷屏、队列静默丢弃、无轮转 |
| 文档/配置不一致 | 4 | 中 | `General.Debug`、`Diagnostics.ResourceView` 实际无作用 |

**总体判断：** 工程分层清晰，代理/主体/帧后端职责边界正确；GPU 同步契约（票据、fence、slot 退役、资源保留）设计完整，未发现每帧必然的 CPU 死等。主要问题集中在三处：

1. **一把全局递归锁同时保护面板注册表与整帧渲染**，窗口消息处理会被整帧 NR 工作阻塞（`panel.cpp:327` 与 `:1285`）。该问题**只在"消息泵与 Present 不同线程"的游戏上成立**，同线程拓扑下无影响；需先按 PF-1 的方法实测确认，再决定是否拆锁。
2. **NR 活跃时每帧存在约 5 次全屏资源复制**外加 2 次 D3D11 `Flush` 和 3 次 fence signal，4K 下带宽与提交开销明显。
3. **错误/等待路径的资源处置过重**：录制票据被判为 Discarded 时会销毁整个 `Context`（模型 feature + 全尺寸工作纹理），而 `ModelRuntime` 一旦绑定过设备就再也无法换设备。

---

## 3. 性能问题

| # | 等级 | 位置 | 问题 |
| --- | --- | --- | --- |
| PF-1 | 高（条件） | `panel.cpp:304`、`panel.cpp:327`、`panel.cpp:1285-1296` | 面板全局锁在整帧渲染期间被持有，窗口消息被阻塞；**仅在"消息泵与 Present 不同线程"时成立** |
| PF-2 | 高 | `bridge.cpp:291-299`、`diagnostics.cpp:260` | 每帧约 5 次全屏 `CopyResource` |
| PF-3 | 高 | `depth11.cpp:97-110`、`depth11.cpp:248-262` | 每帧多张全分辨率深度 `CopyResource` + O(n²) 扫描 |
| PF-4 | 中 | `bridge.cpp:295`、`:317`、`:333` | 每帧 2 次 `Flush` + 3 次 signal + 1 次队列 Wait |
| PF-5 | 中 | `depth11.cpp:194-205` | 每次 Draw 都取全局递归锁并做 map 查找 |
| PF-6 | 中 | `input_keyboard.cpp:69-91`、`input.cpp:206-210` | `GetAsyncKeyState`/`GetKeyState` 每次调用都取全局 mutex |
| PF-7 | 中 | `dense_flow.cpp:111-147`、`dense_flow.hlsl:245-284` | 估算 MV 每帧约 24 次绘制，含 2 次全屏 pass 与 2 次全屏 copy |
| PF-8 | 中 | `lite_meter.hlsl:1-12`、`pass.cpp:769-790` | 白点计量一次 26.2 万次点采样，Measured 模式下即使面板关闭也常驻 |
| PF-9 | 低 | `panel.cpp:1211-1212` | 插件空闲时仍每帧两次加锁 + map 查找 |
| PF-10 | 低 | `panel.cpp:861`、`:478`、`:642` | 面板每帧多次堆分配（wstring 复制、UTF-16→UTF-8） |
| PF-11 | 低 | `bridge.cpp:327-341`、`panel.cpp:1243-1248` | `Drain()` 最长 5 秒 CPU 等待，挂在 resize/析构路径 |
| PF-12 | 低 | `bridge.cpp:239-251`、`dense_flow.cpp:62-80` | 格式/尺寸变化时整体重建槽位与约 42 张纹理 |

### PF-1 面板全局锁覆盖整帧渲染（高，条件成立）

**先决条件：线程拓扑。** 这条问题是否成立，完全取决于游戏把消息泵和 `Present` 放在同一个线程还是不同线程，静态代码无法判断：

> 锁**不增加** CPU/GPU 工作量，也不增加帧时间；它只把"消息派发"从"可与 NR 处理并行"变成"必须排在 NR 之后"。所以本节讨论的额外延迟是**等待**，不是成本。

| 拓扑 | `WindowProc` 与 `Present` 的关系 | 实际影响 |
| --- | --- | --- |
| A. 同线程（消息泵与 Present 都在主线程） | 消息只在帧间被 `DispatchMessage` 派发，此时锁空闲；同线程重入 `recursive_mutex` 不阻塞 | **无**。此时延迟来自"帧变长"，与锁无关 |
| B. 分离线程（主线程 pump 消息，渲染线程 Present） | 两个线程争同一把锁，`WindowProc` 会等渲染线程释放 | **有**，本节所述现象成立 |

UE 的 RHI 线程、id Tech 等属于 B；Unity 与多数自研引擎默认属于 A。因此**动手前先测量**：在 `WindowProc` 取锁前后各取一次 `QueryPerformanceCounter`，记录原子最大值（并记录当前正在进行的锁内阶段），跑一次目标游戏：

- 最大值 ≈ 0 → A 类，PF-1 降级为无影响，不应改。
- 最大值达到几毫秒且与 NR 帧时间同量级 → B 类，值得按下面的建议改。

`DrawSwapchain`（`panel.cpp:1285`）先取 `Global().mutex`，再调用 `Panel::Draw`；后者执行 `NativeFrame11` → 桥接 → 模型评估 → ImGui 渲染。`WindowProc`（`panel.cpp:324`）、`RouteKeyboard`（`:304`）和消息队列钩子都要求同一把锁。

窗口子类在交换链包装时（`Panel` 构造）就已安装，**与面板是否打开无关**；因此每条窗口消息都会先取这把锁。

三种状态的实际开销必须分开看：

| 状态 | 锁内每帧工作 | 对输入/手感的实际影响 |
| --- | --- | --- |
| 面板关闭 + NR 关闭（默认） | `RetireCompleted()`（一次 `parkedMutex` + 空表扫描后 `modelCached==false` 立即返回）+ `NativeFrame11()`（4 个原子读后立即返回） | 微秒级；窗口消息最多等这几微秒，**无感知** |
| 面板关闭 + NR 开启 | `NativeFrame11` 全流程：深度候选备份、桥接复制、模型评估、结果回拷 | 每条窗口消息都可能等一整帧 NR 工作；首次创建 feature/PSO/纹理时可达数十至数百毫秒 → **有感知** |
| 面板打开 | 上者 + ImGui 帧（资源页还会额外跑光流估计与 3 格预览） | 同上，且更重；鼠标/键盘重放与绘制都要排队 |

所以"影响手感"只对后两行成立；默认配置（`NeuralRendering.Enabled=0` 且面板不打开）不成立。真正的耦合点是：**NR 一旦开启，它就跑在 Present 线程并且整帧持有面板锁**，与面板开不开无关。

等待时长的含义（消息泵视角）：

```
渲染线程 (Present):  [取 Global().mutex][ 深度备份 → 桥接复制 → NGX Evaluate → 合成 → 回拷 ]   [释放]
                                                                                ↑
GUI 线程 (消息泵):            WM_MOUSEMOVE / WM_INPUT 到达                            │
                              尝试取锁 (panel.cpp:327) ── 阻塞 ─────────────────────┘
                              拿到锁 → 转发游戏原 WndProc
```

"等一整帧"= 等待上限是渲染线程**这一帧剩余的锁内工作时间**，不是固定值：锁空闲时到达则零等待，刚被取走时到达则等满该帧剩余部分（首次创建 feature/PSO/纹理时可达几十至数百毫秒，稳态为该帧 NR 处理时间）。`std::recursive_mutex` 不保证 FIFO，渲染线程下一帧会再次抢锁，因此极端情况下 GUI 线程可能连续等两帧以上，但不会随运行时间累积。消息不会丢失，只是延迟。

只影响"经消息泵取输入"的路径：`WndProc` 内处理输入、或依赖 `WM_INPUT` 派发的游戏会被延迟；每帧自行 `GetAsyncKeyState`/DirectInput 轮询的游戏不经过这把锁（那条路径只有 `input_keyboard` 自己的小锁，见 PF-6）；`PeekMessage` 路径中只有键盘消息会进 `RouteKeyboard` 取锁，鼠标消息在 `panel.cpp:300-303` 就被过滤返回。

- 残余风险：如果游戏在自己的 `WndProc` 里等待渲染线程（部分引擎会），仍存在循环等待，只是 `WindowProc` 现在会在调用原 `WndProc` 前解锁，把死锁概率降低了。
- 建议（仅 B 类需要做）：拆成两级锁——注册表锁只用于查表/取引用，`Panel` 自己的锁保护 Impl/ImGui 状态，`Panel::Draw` 的 NR 工作不再持有注册表锁：
  ```
  DrawSwapchain: 锁注册表 → 取 shared_ptr<Panel> → 解锁 → panel->Draw()
  WindowProc:    锁注册表 → 查 route 并取引用 → 解锁 → 锁 panel 状态锁 → 处理 → 解锁 → 原 WndProc
  ```
  代价是 `WM_NCDESTROY` 的 `Impl` 生命周期要改成 `shared_ptr` 持有（现在靠解锁后重新校验 route）。这与 DX12 工程的处理方式一致。
- 无法修的部分：NR 必须在 Present 所在线程、用 D3D11 立即上下文做完（桥接复制、NGX Evaluate、回拷）。挪到其它线程等于引入一帧额外延迟加更多同步，不划算。所以 B 类游戏在 NR 开启时**帧时间本身会变长**，那部分只能靠 PF-2 / PF-4 / PF-7 降成本；拆锁只是不让窗口消息再被额外拖住。
- 一次性（非每帧）阻塞点也在同一把锁下：`TrackSwapchain` 会创建 ImGui 上下文并从 `C:\Windows\Fonts\segoeui.ttf` 读字体、`depth::Install` 会建 deferred context 并装 15 个 detour、`ResizeBuffers` 全程持锁、`BeforeResize` 可能触发最长 5 秒的 `Bridge11::Drain`。这些只发生在启动/切分辨率/退出。

### PF-2 每帧约 5 次全屏复制（高）

NR 活跃时单帧的资源流动：

| 次序 | 复制 | 位置 |
| --- | --- | --- |
| 1 | 颜色 `CopyResource`（D3D11） | `bridge.cpp:218` |
| 2 | 深度 `CopyResource`（转换时先整体备份再 compute） | `bridge.cpp:219` |
| 3 | MV `CopyResource`（同上） | `bridge.cpp:219` |
| 4 | 模型结果 → 共享输出（DX12 `CopyResource`） | `bridge.cpp:299` |
| 5 | 共享输出 → 游戏颜色纹理（D3D11 `CopyResource`） | `diagnostics.cpp:260` |

3840×2160：RGBA16F 单张约 63 MiB，RGBA8 约 32 MiB。第 1/4/5 次就接近 190 MiB/帧；60 fps 约 11 GB/s 纯复制带宽，尚未计入转换 pass 与模型自身的读写。

- 建议优先消除第 5 次复制：在模型最终合成 pass 里直接写桥接输出（当前 `pass.cpp` 已经先写 `ctx->output`，再让 `bridge.cpp:299` 复制一次），或让 `result` 直接指向共享纹理。
- 深度/MV 的格式转换（`bridge_convert.hlsl`）当前是"先 `CopyResource` 到 backup，再 compute 写出 twin"；可在同一个 compute 里直接从源资源读取（`Texture2D` SRV），省掉一次全屏复制。

### PF-3 深度候选备份（高，条件触发）

`depth::EndFrame`（`depth11.cpp:248`）在 `capture=true` 时遍历所有候选（上限 32），对每个通过 `Shape` 与顶点数过滤的候选调用 `Backup`（`:97`）。而 `Backup` 内部又会重新计算 `Top(owner)` + `TrimCopies(owner, top)`，即每次调用都 O(n) 扫描全表：

- 每帧最坏 32 次全分辨率 `CopyResource`；
- 叠加 `Top`/`TrimCopies` 的 O(n²) 遍历；
- `Binding`/`BeforeBind`/`Cleared`（`:179`、`:185`、`:203`）在每次 DSV 切换/清空时还会再触发一次 `Backup`，形成"每段一个全尺寸拷贝"。

只在原生捕获路径（`[NeuralRendering] NativeCapture=1` 且需要 NR 或调试视图）触发。该项默认已改为 `0`，需要时在 INI 中显式开启并重启游戏。建议：每帧只备份最终选中的 1 个候选（或在 `EndFrame` 里先算 `best` 再备份），并把 `Top` 结果缓存到帧级变量。

### PF-4 每帧 D3D11 提交开销（中）

`Bridge11::Impl::Process` 每帧执行：`Signal(fence11)` + `Flush`（`:295`）、`Wait(fence11)` + `Signal(fence11)` + `Flush`（`:317`），失败路径还有 `Flush`（`:333`）。

`ID3D11DeviceContext::Flush` 会把命令缓冲立刻交给内核，等于强制打断驱动的批处理。每帧 2 次是明确的 CPU/驱动开销。建议至少合并第二次 `Signal`（结果 fence 可以直接用于下一帧的 slot 完成判定，不必再额外 signal 一次用于 `Wait`）。

### PF-5 逐 Draw 取全局锁（中）

`depth::Draw`（`depth11.cpp:194`）每次绘制都 `lock_guard(R().mutex)` 并调用 `RegisterLocked(context)`（内含 `std::map::find`）。一个 5,000 draw/frame 的游戏即 5,000 次全局递归锁 + map 查找。`TrackingEnabled` 已经做了线程局部原子缓存（`:141`），但 `Draw` 路径没有复用。

建议把 `Context*` 缓存在 thread_local（像 `TrackingEnabled` 一样用 `policyEpoch` 失效），避免逐 draw 的加锁与查找。

### PF-6 输入轮询 Hook 取锁（中）

`PanelKeyboardPollBlocked`（`input_keyboard.cpp:69`）在非内部调用时对每个 `GetAsyncKeyState`/`GetKeyState` 取一次全局 mutex；`PanelKeyboardPollMask`（`:80`）在每次 `GetKeyboardState` 里遍历 256 键并对每个持有的键调用 `GetAsyncKeyState`。游戏习惯每帧多次轮询按键/鼠标，这条路径会把输入线程与面板线程串行化。

建议：把 `pollRelease` 用原子位图（256 位）表示，热路径无锁读取，只在置位/清位时加锁。

### PF-7 估算光流成本（中）

`DenseFlow12::Record` 每帧固定约 24 次绘制 + 2 次全屏拷贝：`Prepare`（全屏）、`DepthMip`×3、`Down`×7、`Match`+`Pool`×8（在 1/8 分辨率）、`Upscale`×3、`Finish`（全屏）、`RecordForNR` 再加 `Pixels`（全屏）+ 2 次全分辨率 `CopyResource`（`dense_flow.cpp:145`）。

`Match` 内层是 `MATCHING_SAMPLES=16` 次 GatherRed，梯度下降最多 32 步（`dense_flow.hlsl:245-284`），Quality 直接决定该步数。这是原生 DX11 路径的主要 GPU 开销；仅在游戏不提供 MV 时触发。建议在面板中把 Quality 的默认值保持 Low，并把 `num_steps` 上限按分辨率下调。

### PF-8 白点计量（中）

`lite_meter.hlsl` 每个线程读 64 个样本，64×64 个线程 → 每次 262,144 次全屏点采样。`pass.cpp:771` 的条件是 `modelMeasurement || settings.panelStatistics`，其中 `WhiteMode::Measured`（或 Automatic 且无 game exposure 时）即使面板关闭也成立，`WhiteMode::Game + reverseRatio` 同理（`:766-769`）。

建议：把默认 `WhiteEvery` 提高（当前 4），或在 Game 模式下直接用完成的 1×1 曝光回读，不必每 4 帧扫全屏。

### PF-9 空闲时的每帧开销（低）

`Panel::Draw` 无条件调用 `nr::RetireCompleted()`（`panel.cpp:1211`）和 `nr::NativeFrame11()`（`:1212`）。前者每次都取 diagnostics 的全局锁并调用 `RetireModelFeatures()`（再取一次 `parkedMutex`）。

建议把 `RetireCompleted` 的即时返回条件提前到调用点：`if (ModelRequested())` 才进入。

### PF-10 面板每帧堆分配（低）

`ConfigStorageSnapshot()`（`panel.cpp:861`）克隆 3 个 `std::wstring`；`Utf8(model.reason)`、`Utf8(resources.reason)` 每帧做 UTF-16→UTF-8 转换；`GetModelSettings()` 在两个 Tab 各取一次。数量不大，但都是可以静态缓存或改成 `string_view` 的。

### PF-11 resize/析构阻塞（低）

`Bridge11::~Bridge11` → `Drain()`（`bridge.cpp:327`）会 `SetEventOnCompletion` + `WaitForSingleObject(..., 5000)`。该析构出现在 `BeforeResize`（`panel.cpp:1243-1248` → `ReleaseNative11`）和面板销毁路径上，且持着面板锁与 diagnostics 锁。GPU 繁忙时最坏 5 秒卡顿（只在 resize/退出时）。

### PF-12 变化即重建（低）

`Bridge11::Matches` 不匹配时新建整套槽位（含 3 张输入 twin、输出、preview），`DenseFlow12::Ensure` 不匹配时新建约 42 张纹理。分辨率/格式稳定后不再发生，但 `ResizeBuffers` 或游戏切换 HDR 时会一次性付出分配代价，且旧槽位要等 fence 才回收。

---

## 4. 逻辑与正确性问题

| # | 等级 | 位置 | 问题 |
| --- | --- | --- | --- |
| LG-1 | 高 | `pass.cpp:386-403` | 未提交录制（Discarded）会销毁整个 Context，包括模型 feature |
| LG-2 | 中 | `model_runtime.cpp:78` | 绑定过设备后无法换设备，NR 永久停在 E_NOTIMPL |
| LG-3 | 中 | `runtime.cpp:61`、`diagnostics.cpp:390`、`panel.cpp:1044` | `General.Debug` 与 `Diagnostics.ResourceView` 实际无作用 |
| LG-4 | 中 | `diagnostics.cpp:149-153` | 光流质量变更只失效 NR 历史，不失效光流历史 |
| LG-5 | 中 | `pass.cpp:451` + `:510` | 白点 `Resolve` 每帧算两次，第一次用 `dt=0` |
| LG-6 | 中 | `pass.cpp:532-538`、`:454-492` | 调参变化不推进 `whiteEpoch`，旧计量的读回会被用于新参数 |
| LG-7 | 低 | `pass.cpp:263-268` | `gpuTiming` 关闭时清空 `timingPool`，与"录制持有查询"的注释语义冲突 |
| LG-8 | 低 | `pass.cpp:269-284` | `refuse()` 不清零 `readyPasses/failedPass`，状态可能残留上一帧 |
| LG-9 | 低 | `pass.cpp:445`、`:527`、`:536` | context 重建与 `reset` 分支对 `frameIndex` 的处理不一致 |
| LG-10 | 低 | `hooks.cpp:202-203`、`:145` | `captureDepth>=4` 与重复包装静默放弃，无日志 |
| LG-11 | 低 | `depth_hooks.cpp:66-73` | 每次 `Install` 都 `CreateDeferredContext(0)` 探测 vtable |

### LG-1 Discarded 分支拆除整个上下文（高，错误路径）

`pass.cpp:386` 判断上一帧录制票据为 `Discarded` 时执行：

```
previous.reset(); context.reset(); white.Reset(); ++whiteEpoch; ...
```

`context` 持有 `Layer`（模型 feature）与 `proxy/answer/output/guides` 四类全尺寸纹理。`context.reset()` 触发 `Layer::~Layer` → feature 被 park，票据为 Discarded 时立即 `Release`；下一帧必须重新 `CreateFeature` + 重新分配全部工作纹理。

触发条件：pass 返回 `S_OK` 但桥接随后失败（结果校验失败、提交 signal 失败、设备移除），即票据被判为 Discarded。

影响：一次偶发失败导致用户可见的长时间停顿（feature 重建）与显存抖动。资源本身从未被 GPU 引用，本可以只丢弃"历史"（`white.Reset()`、`domainKnown=false`、`previous.reset()`），保留 `context` 与 `Layer`。建议区分 `InvalidateHistory()`（历史失效）与 `Reset()`（资源销毁），错误路径只做前者。

### LG-2 设备变更后 NR 永久失效（中）

`ModelRuntime::Initialize`（`model_runtime.cpp:78`）一旦 `device` 已设置，后续任何其他 `ID3D12Device*` 都直接返回 `E_NOTIMPL`。设备丢失恢复或适配器切换后：

- `ModelActive()` 仍为真（`unavailable` 未置位）；
- 桥接每次新建 transport 设备，`Initialize` 返回 `E_NOTIMPL`，`Model11` 直接返回，NR 永久不工作；
- 面板只会显示 "NR runtime unavailable for this device"（`pass.cpp:294`），不会尝试复位。

建议：在识别到设备不匹配时把 `device/unavailable` 收敛为可重试状态（`RetryFailedInitialization` 路径已有参数块销毁逻辑），或至少在状态里区分"设备不匹配"并提示用户切换 NR 开关。

### LG-3 两个配置键无效（中）

- `runtime.cpp:61` 仍然读取 `[General] Debug`（`status.debug`），其消费者只剩启动横幅里的 `debug=%u`（`runtime.cpp:92-96`）与两处 `OutputDebugStringW`（`:97`、`:104`）；原先唯一读取它的 `lite::DebugOutput()` 已在清理中删除。因此该键对游戏内行为没有可见影响。
- `[Diagnostics] ResourceView` 在 `runtime.cpp:67` 读入并 `SetView(view,false)`，但 `SetView` 只在 `persist=true` 时写回（`diagnostics.cpp:390-394`），而所有调用点都传 `false`；面板每次打开后 `panel.cpp:1044` 强制 `SetView(Off,false)`。因此该键既不会保存也不会生效。

README 第 36 行声称"旧 `General.Debug`、`Diagnostics.Capture`、`Resources.DepthTracking` 键不再读取"——后两者确实消失，`General.Debug` 仍在读取（只影响启动横幅与调试输出串）。建议：要么删掉该键，要么恢复其含义并写入文档。

### LG-4 光流质量变更不失效光流历史（中）

`diagnostics.cpp:149`：

```cpp
if (estimate && native.modelFlowOptions != options) {
    native.model->InvalidateHistory();
    native.modelFlowOptions = options;
}
```

只把 NR 模型历史标记为重建，`DenseFlow12` 的 `Generation::frames` 没有归零，`Ensure` 也只在尺寸/翻转变化时重建。切换 MV quality 后，光流会拿旧 quality 产生的上一帧金字塔当先验。建议同时 `modelFlow->InvalidateHistory()`。

### LG-5 白点每帧算两次（中）

`pass.cpp:451` 先 `white.Resolve(settings.white, exposure, 0)`（`dt=0`），`:510` 再用真正的 `dt` 与（可能是反向比率的）`whiteSettings` 算一次。第一次的副作用是 "`!started` → 直接吸附到 target"，会让时间滤波在 reset 后的第一帧失效，同时多一次排序/统计计算。建议删除第一次调用，把 `!started` 的吸附逻辑保留在第二处。

### LG-6 调参变更不推进 `whiteEpoch`（中）

`retire()`（`pass.cpp:454-492`）要求 `completed->whiteEpoch == whiteEpoch && !reset` 才消费计量回读。调参（`SameTuning` 变化，`:532-538`）只重置了 `layer`，没有 `++whiteEpoch`。于是切换 Intensity/Style 后，上一套参数下的 64×64 白点/曝光回读仍会被当作新参数的测量结果消费一次，造成一帧白点跳变。

### LG-7 / LG-8 / LG-9 状态字段细节（低）

- `pass.cpp:263`：`!settings.gpuTiming` 时 `timingPool.clear()`，但注释（`:262`）说"在途录制保留自己的查询"，两者不矛盾（共享指针还在），只是每帧重建 pool 数组本身没有意义。
- `pass.cpp:269-284`：`refuse()` 只设置 `ready/error/reason/modelResult`，`readyPasses/actualPasses/failedPass` 保留旧值，面板可能短暂显示"上一帧的 pass 数"。
- `pass.cpp:527`（context 重建）把 `frameIndex=0`，而 `:445` 的 `reset` 分支只重置白点不重置 `frameIndex`，导致 `white.every` 的采样相位在 reset 后错位。属于表现细节。

### LG-10 / LG-11（低）

- `hooks.cpp:145`、`:202-203`：`captureDepth >= 4` 时直接 `return` 不做包装，也没有日志；递归嵌套过深时表现为"插件静默失效"，难以排查。
- `depth_hooks.cpp:68`：`Install` 每次（每个 Panel 创建）都 `CreateDeferredContext(0)` 只为拿 vtable 做探测，之后丢弃。可改用 `ID3D11DeviceContext1` 静态表或缓存结果。

---

## 5. 多余代码（死代码 / 只写不读 / 遗留）

| # | 位置 | 内容 | 状态 |
| --- | --- | --- | --- |
| DC-1 | `hooks.cpp`（原 `:209`、`:228`） | `bool hooksAdded` 赋值后从未读取 | 已清理 |
| DC-2 | `input.cpp`（原 `:197`） | `MouseKey(int)` 函数，无调用 | 已清理 |
| DC-3 | `pass.hpp` | `PassStatus::timedPasses`，只写不读 | 已清理 |
| DC-4 | `pass.hpp` | `PassStatus::whiteMeasurementPending`，只写不读 | 已清理 |
| DC-5 | `depth11.hpp` | `Result::candidates/draws/indirect/preserved`，只写不读 | 已清理 |
| DC-6 | `pass.cpp` / `pass.hpp` | `PassExecutor12::Reset()`，无调用者 | 已清理 |
| DC-7 | `dense_flow.hpp` / `.cpp` | `DenseFlow12::MotionUV()`，无调用者 | 已清理 |
| DC-8 | `panel.cpp`、`runtime.cpp/.hpp`、`graphics.hpp` | `GraphicsSnapshot()`、`DebugOutput()`（含 `debugOutput` 原子量） | 已清理 |
| DC-9 | `preview.cpp` | 空匿名命名空间 | 已清理 |
| DC-10 | `pass.cpp`、`diagnostics.cpp` | 7 处空代码块 / 空 `if` 分支 | 已清理 |
| DC-11 | `CMakeLists.txt` | `d3dcompiler`、`dwmapi` 链接（源码无 `D3DCompile*`、无 `Dwm*`） | 已清理 |
| DC-12 | `assets/iMMERSE_bluenoise_opt.png` | 未被任何脚本/CMake 引用 | 保留（是 `flow_noise.r8` 的来源参考图） |
| DC-13 | `tools/deploy.ps1` | 硬编码本机游戏路径（`F:\SteamLibrary\...`、`H:\Games\...`） | 保留（部署脚本的功能数据） |
| DC-14 | `panel.cpp`（`SliderTip` 表） | 部分提示与 `SliderHelp` 重复 | 保留（可读性，非死代码） |

说明：`LITE_TEST_HOOKS` 条件编译块已经不存在；`dist/`、`build/` 已被 `.gitignore` 排除，但仍存在于工作目录（含旧 DLL 与日志）。

---

## 6. 日志问题

| # | 等级 | 位置 | 问题 |
| --- | --- | --- | --- |
| LOG-1 | 中 | `model_settings.cpp:57-62` | 等待原因抖动会逐次写日志 |
| LOG-2 | 中 | `runtime.cpp:112-118` | 队列满 4096 行后静默丢日志 |
| LOG-3 | 低 | `runtime.cpp:24-48` | 日志文件永不关闭/轮转 |
| LOG-4 | 低 | `runtime.cpp:61`、`:97`、`:104` | `OutputDebugString` 依赖无实际作用的 `Debug` 键 |
| LOG-5 | 低 | `diagnostics.cpp:44-62` | `Report()` 在两种失败码之间来回时每帧写一行 |
| LOG-6 | 低 | `input.cpp:278`、`model_runtime.cpp:94` | 启动期一次性提示，可保留，但格式分散（`Log(L"...")` vs `swprintf_s`） |

### LOG-1 等待原因刷屏（中）

`ReportModelWait`（`model_settings.cpp:57`）在 `reason` 字符串变化时 `Log`。`PublishPass` 每次成功评估都会用 `nullptr` 清除，而暂停菜单、DLSS 开关、场景切换会让状态在"处理中 / 等待输入 / 等待深度"之间来回切换，日志会按帧率刷行。建议对同一原因加 1 秒节流，或只在首次出现时记录。

### LOG-2 静默丢弃（中）

`runtime.cpp:114`：`if (queue.path.empty() || queue.lines.size() >= 4096) return;`——写满后直接丢，用户与开发者都看不到"日志已截断"。建议计数并在恢复时写一行统计。

### LOG-3 无轮转（低）

`_wfsopen(..., L"a, ccs=UTF-8", _SH_DENYNO)` 常开句柄，进程生命周期不关闭、不按大小切分。长时间游戏会持续增长。建议超过 8–16 MB 时截断/换名。

### LOG-5 状态抖动写日志（低）

`Report()` 用 `reportedCapture != hr` 去重，但失败码若在两值间交替（例如 `E_NOTIMPL` / `E_INVALIDARG`），就会持续写。建议改成"同一 HRESULT 连续失败只写一次，成功后再复位"。

---

## 7. 文档与配置不一致

| 文档 | 声明 | 代码实际 |
| --- | --- | --- |
| `README.md:36` | 旧 `General.Debug` 键不再读取 | `runtime.cpp:61` 仍读取 |
| `README.md:36` | 面板关闭后停止调试工作 | 成立（`DebugActive()` 需 `PanelVisible`），但 NR 与白点计量按启用状态继续 |
| `DLSSNR_LiteDX11.ini` | `[Diagnostics] ResourceView` 可选视图 | 从不持久化，面板打开后强制 Off |
| `CODE-REVIEW-2026-10-12.md` | 第 3 条"风格切换只重建 Layer" | 成立（`pass.cpp:532-538`） |
| `CODE-REVIEW-2026-10-12.md` | 第 6 条"全部 shader 已预编译" | 成立（`CMakeLists.txt:56-70`，源码无 `D3DCompile`） |
| `PERFORMANCE-REPORT.md` | "没有发现每帧必然 CPU 阻塞" | 成立（`Process` 不等待），但未覆盖 PF-1 的锁串行化与 PF-2 的复制量 |

两份历史报告（`CODE-REVIEW-2026-10-12.md`、`PERFORMANCE-REPORT.md`）仍然保留在工程根目录，结论与当前代码有部分重叠但缺少本报告中的锁串行化、全屏复制量化、Discarded 分支拆除上下文等项。建议合并归档，避免第三份报告继续累积。

---

## 8. 建议修复顺序

1. **PF-1**（锁串行化）— 先测线程拓扑：A 类（消息泵与 Present 同线程）下无影响，跳过；B 类才值得拆锁，改动集中在 `panel.cpp` 的 `WindowProc` / `DrawSwapchain` / `Panel::Draw` 三个入口。注意 NR 在 Present 线程内执行这一点不可改，帧时间只能靠 PF-2/PF-4/PF-7 降低。
2. **PF-2 / PF-3**（全屏复制）— 收益最大，且可以只改 `bridge.cpp` / `pass.cpp` / `depth11.cpp` 的资源流向。
3. **LG-1 / LG-2**（错误路径的资源与设备生命周期）— 影响长时间稳定性。
4. **PF-5 / PF-6**（逐 draw / 逐轮询取锁）— 低风险、可量化。
5. **DC-12 / DC-13**（遗留资源与本地路径）与 LG-3（配置键）— 一次性清理，无行为变化。
6. **LOG-1 / LOG-2**（日志节流与截断提示）。
7. 其余低等级项按注释整理即可。

---

## 9. 验证与限制

- 以 MSVC（VS 18 Professional）`/W4 /EHsc /std:c++20 /MT /O2` 单独编译了 18 个 `.cpp`：`hooks`、`panel`、`input`、`input_keyboard`、`input_directinput`、`runtime`、`config_storage`、`capture`、`model_runtime`、`model_settings`、`depth_hooks`、`dense_flow`、`preview`、`white_point`、`pass`、`bridge`、`diagnostics`、`depth11`；未编译 `bootstrap`、`config_writer`、`proxy`、`proxy_forward`、`caller`（这 5 个不含本次结论涉及的逻辑）。结果：**无警告、无错误**。因此 DC-1/DC-2 这类"死变量/死函数"不会被编译器提示，需要人工清理。
- 未执行 `Build.ps1`（避免覆盖 `dist/` 中已部署的二进制）、未启动游戏、未做 GPU trace。PF-* 的量化（MiB/帧、GB/s）是按 3840×2160 与 RGBA16F/RGBA8 的格式大小推算，不是实测。
- 建议在真实目标机上按 `PERFORMANCE-REPORT.md` 第 4 节的方法分别测量"仅 NR"、"原生深度/MV 估算"、"资源面板打开"三种状态，用面板已有的 NR GPU 计时先确认 PF-4/PF-7 的量级，再用 PIX/GPUView 拆分 PF-2 的复制成本。

---

## 10. 清理记录（2026-10-25）

### 10.1 已删除内容

| 文件 | 删除内容 | 备注 |
| --- | --- | --- |
| `src/hooks.cpp` | `bool hooksAdded`（1 处声明 + 1 处赋值） | `AttachFactory` 内从未读取 |
| `src/input.cpp` | 匿名命名空间中的 `MouseKey(int)` | 全文无调用 |
| `src/nr/pass.hpp` | `PassStatus::timedPasses`、`PassStatus::whiteMeasurementPending` | 只写不读 |
| `src/nr/pass.cpp` | 对应的 3 处赋值语句 | 同上 |
| `src/nr/pass.hpp`、`src/nr/pass.cpp` | `PassExecutor12::Reset()` 声明与定义 | 无调用者 |
| `src/nr/depth11.hpp`、`src/nr/depth11.cpp` | `depth::Result::candidates/draws/indirect/preserved` 及 4 处赋值 | 只写不读；`image/identity/inverted/deferred` 保留 |
| `src/nr/dense_flow.hpp`、`src/nr/dense_flow.cpp` | `DenseFlow12::MotionUV()` | 无调用者；`Generation::uvMotion` 仍被 `RecordForNR` 使用，保留 |
| `src/runtime.hpp`、`src/runtime.cpp` | `DebugOutput()` 及 `debugOutput` 原子量（含初始化赋值） | `Status::debug` 仍保留，用于启动横幅与 `OutputDebugStringW` |
| `src/graphics.hpp`、`src/panel.cpp` | `GraphicsSnapshot()` 声明与定义 | 无调用者 |
| `src/nr/preview.cpp` | 空匿名命名空间 | — |
| `src/nr/pass.cpp` | 2 处空代码块 | — |
| `src/nr/diagnostics.cpp` | 4 处空代码块 + 1 处空 `if` + 1 处空 `else if` 分支 | `Capture11` 的 `if/else if` 链改写为单个 `if` |
| `CMakeLists.txt` | 两处 `d3dcompiler`、一处 `dwmapi` 链接项 | 工程自身不调用 `D3DCompile*`/`Dwm*`；ImGui 后端源码内仍有 `#pragma comment(lib, ...)`，因此不改变最终导入表 |

### 10.2 未删除（附理由）

| 项 | 理由 |
| --- | --- |
| `assets/iMMERSE_bluenoise_opt.png` | `assets/flow_noise.r8` 的来源参考图，删除会丢失可再生成性；如需瘦身可移到 `docs/` 并在 README 说明 |
| `tools/deploy.ps1` 中的本机路径 | 是部署脚本的功能数据（`-Target bg3/roe/...`），不属于运行时代码；建议改为外部配置或参数默认值 |
| `SliderTip` / `SliderHelp` 的重复文案 | 属于可读性问题，删除会影响用户提示 |

### 10.3 验证

- 在独立目录 `build/deadcode-check` 与 `build/deadcode-check2` 用 CMake + Ninja + MSVC 做了**两次全新配置 + 全量 Release 构建**：39 个构建步骤全部成功，**0 警告 0 错误**，`dxgi.dll`、`version.dll`、`winmm.dll`、`nvngx.dll_litedx11.dll` 均链接成功。
- 未执行 `cmake --install`，`dist/Release`、`dist/Debug` 中已部署的二进制**未被覆盖**，现有 `build/windows-release` / `build/windows-debug` 也未改动。验证用的两个临时构建目录已删除。
- 删除后再次全仓检索：`timedPasses`、`whiteMeasurementPending`、`MotionUV`、`DebugOutput`、`GraphicsSnapshot`、`hooksAdded`、`MouseKey`、`d3dcompiler`、`dwmapi` 在 `src/` 与 `CMakeLists.txt` 中已无残留引用。
- 行为影响评估：删除项均为无读取者的字段、无调用者的函数、空语句块与未被工程使用的链接项。`PassStatus`、`depth::Result` 只在进程内使用，不存在跨模块 ABI 交换，因此结构体成员的删除不影响二进制兼容性。仍建议在真实游戏中回归一次 NR 启用/关闭、原生深度捕获与资源面板三条路径。

### 10.4 配置键改名（2026-10-25，同日）

| 项目 | 旧 | 新 |
| --- | --- | --- |
| 键名 / 分区 | `[Resources] NativeDX11` | `[NeuralRendering] NativeCapture` |
| 代码默认值 | `1`（启用） | `0`（关闭，需显式开启并重启） |
| 旧键回退 | — | 无（按用户要求不兼容旧配置） |

- 该键同时控制深度追踪 hook 的安装、原生场景捕获输入，以及 Resources 页的 Depth/MV 预览；因 hook 在面板构造时安装，改动需重启游戏。
- 面板 Resources 页新增提示：关闭时说明"预览为空、NR 需要游戏 NGX 输入、需重启"，不再只显示 `Missing`。
- 四个已部署目录（`roe`、`bg3`、`wildlands`、`bannerlord`）的 INI 已迁移为 `NativeCapture=1`，保持原有行为。
