# 第三方依赖

| 依赖 | 用途 | 内容与许可证 |
| --- | --- | --- |
| Dear ImGui 1.90.9 | 面板 | 原始核心、官方 Win32／DX11／DX12 后端；imgui/LICENSE.txt，MIT |
| Microsoft Detours 4.0.1 | 创建入口、键鼠 Hook | 原 Lite／Opt 的头文件、x64 静态库；detours/LICENSE.txt，MIT |

Vendor 源码保持原样。官方手柄加载／轮询通过 `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD` 禁用；Detours 静态链接，无额外 Hook DLL。

`src/wrappers.hpp` 提取旧 Lite COM 转发机制，保留 GPL 来源声明，项目许可证为根目录 LICENSE。

依赖许可说明合并于 [NOTICES.txt](NOTICES.txt)，来源与差异见 [参考说明](../docs/REFERENCES.md)。发布时只复制合并说明为 THIRD_PARTY_NOTICES.txt，不生成许可证目录。
