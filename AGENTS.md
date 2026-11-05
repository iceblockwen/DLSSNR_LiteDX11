# DLSSNR_LiteDX11 开发规则

本工程是 `DLSSNR_Lite` 的 DX11 链路迁移，采用 `DLSSNR_LiteDX12` 的构建与发布骨架。

## 原则

- 先检查本工程与 `DLSSNR_Lite`、`DLSSNR_LiteDX12` 中的既有实现，再决定修改方式。
- 不修改参考工程；本工程从它们迁移而来，独立演进。
- 本工程只接入 DX11 游戏的捕获、呈现和面板；保持 DX11 NGX 输入经 D3D11On12 桥接到 D3D12 模型执行的资源与同步契约，不添加独立 DX12 游戏捕获路径。
- 真实游戏只由用户运行；常规构建不启动游戏或 GPU 检查。

## 约定

- 窗口、配置、日志和运行时 DLL 使用 `DLSSNR_LiteDX11` / `nvngx.dll_litedx11.dll` 命名。
- `dxgi.dll` 只负责系统导出转发和启动主体，不持有图形、UI 或模型对象。
- 替代入口（`version.dll`、`winmm.dll`，以及后续按同法新增的名字）与 `dxgi.dll` 同一职责：只转发同名系统 DLL 的导出并启动主体，捕获仍靠主体对系统 `dxgi.dll` 安装的 Hook。游戏目录只放一个入口，其余名字留给别的 mod 或 `<名字>-original.dll` 链式加载；导出名与序号以 `src/proxy/<名字>.def` 为准。
- 使用 `Build.ps1` 和 `windows-release` / `windows-debug` CMake presets；发布文件安装到 `dist/<配置>/`。
