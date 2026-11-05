# DLSSNR_LiteDX11

Windows x64 的 DLSS Neural Rendering 插件，按 [DX12 工程](../DLSSNR_LiteDX12) 的 CMake/Ninja presets、PowerShell 构建入口和 `dist/<配置>/` 发布目录组织。

本工程只捕获和呈现 DX11 游戏：包括 DXGI 代理与 SwapChain 面板、D3D11 NGX SR 输入捕获及 D3D11 场景资源诊断。NR 模型通过 D3D11/D3D12 共享纹理和 GPU fence 桥接在内部 D3D12 设备上执行；这属于内部计算后端，不代表支持 DX12 游戏捕获。NR 默认关闭；不提供 NVIDIA runtime 或模型二进制。

NGX SR 捕获覆盖 `NVSDK_NGX_D3D11_EvaluateFeature` 和 `EvaluateFeature_C`，两个入口分别保留原函数与回调。

## 构建

安装 Visual Studio“使用 C++ 的桌面开发”工作负载（MSVC、Windows SDK、CMake 和 Ninja 工具）。

```powershell
./Build.ps1
./Build.ps1 -Configuration Debug
```

输出到 `dist/Release/` 或 `dist/Debug/`。该工程不构建原 Lite 仓库里的测试宿主和测试套件。

插件 HLSL 在配置阶段通过 Windows SDK 的 `fxc.exe` 预编译并嵌入 DLL；缺少 SDK 时构建报错，不回退到渲染线程现场编译。ImGui 官方后端自身的首次着色器编译仍保留。

## 部署

将发布目录中的以下文件放到游戏目录：

- `dxgi.dll`：系统 DXGI 转发代理（默认入口）
- `nvngx.dll_litedx11.dll`：插件主体
- `DLSSNR_LiteDX11.ini`：配置

**入口名三选一。** 游戏目录里只放一个入口 DLL。若 `dxgi.dll` 已被别的 mod 占用（例如黑暗之魂 2 的 PT Mod），改用替代入口 `version.dll`（17 项）或 `winmm.dll`（180 项）：导出名与序号都镜像系统同名 DLL，加载与 Hook 路线不变，DXGI／SwapChain 捕获仍由主体在启动时对系统 `dxgi.dll` 安装，因此不依赖代理名。选哪个取决于游戏实际导入哪个：黑暗之魂 2 的 exe 静态导入 `winmm.dll`，而 `version.dll` 只有在它加载的其它模块引用时才会被带进来。`dist/Release/` 同时产出三种入口，按游戏各取一个。

替代入口按 OptiScaler 的顺序取原 DLL：先找同目录 `<入口名>-original.dll`（留给别的 mod 继续链式转发），没有则用 `System32\<入口名>.dll`。系统同名 DLL 缺失时导出返回 0，并在 `General.Log=1` 时追加一行 bootstrap 失败原因。

NR 还需要兼容的 `nvngx_dlssnr.dll` 模型；游戏不提供 NGX 核心时，还需要 NVIDIA NGX core。Insert 打开面板。日志文件名为 `DLSSNR_LiteDX11.log`。

深度、MV、资源预览、NR 调试画面及 GPU 计时只在面板打开时启用；关闭面板后停止这些调试工作。NR 本身按启用状态继续运行。正式包没有后台深度计数、逐帧调试日志或独立截图开关，旧 `General.Debug`、`Diagnostics.Capture`、`Resources.DepthTracking` 键不再读取。

`Model parameters → Model preset`：Default / 0 由模型决定，A / 1 为原模型路线，B / 2 为新版优化模型。310.9.16.0 的 Default 选择 B；310.8.0.0 没有 B，会回退到其原模型。此项与 `Style` 分开；切换 preset 会重建模型历史。

已登记博德之门 3 的 DX11 部署目录 `F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin`：

```powershell
./tools/deploy.ps1 -Target bg3
```

脚本覆盖插件的主体和所选入口 DLL，已有 DX11 配置和 NR 模型保留，复制后校验 SHA256。`-Entry version|winmm` 改用对应入口；若目录里还留着本插件的另一个入口 DLL，脚本会拒绝部署并提示先删掉。游戏使用 `bg3_dx11.exe` 启动。

ROE 的部署目标为 `roe`，目录 `H:\Games\RiseOfEros`，使用 `./tools/deploy.ps1 -Target roe`。

## 当前迁移范围

- 保留 DX11 NGX 捕获、D3D11/D3D12 共享资源计算桥接、模型调用和资源预览契约；移除独立 DX12 游戏捕获、命令提交跟踪和 DX12 面板路径。
- 不保留 GTA Enhanced 的 DX12 游戏专项分支及游戏命令列表的跨队列提交依赖接口；内部桥接仍使用提交票据、同队列顺序和 fence 完成判定。
- 构建、配置、代理主体命名和发布布局改为 `DLSSNR_LiteDX11`。
- 未迁入原项目的测试、测试宿主、构建产物、部署目录和游戏数据。
