# Microsoft Detours 4.0.1

The headers and x64 static library are copied unchanged from
`DLSSNR_Lite_old/third_party/detours`, which retained the OptiScaler reference's
dependency. No replacement library or binary DLL is introduced.

The MIT license is retained from Microsoft Detours v4.0.1:
https://github.com/microsoft/Detours/blob/v4.0.1/LICENSE.md

Lite follows the legacy Lite/Opt transaction sequence: Begin, UpdateThread, Attach,
Commit, with failure checks and Abort for incomplete groups. Swapchain Present,
ResizeBuffers and Release are reached through COM wrappers rather than native
function detours.
