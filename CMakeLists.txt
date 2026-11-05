cmake_minimum_required(VERSION 3.24)
set(ENV{VSLANG} 1033)
project(DLSSNR_LiteDX11 VERSION 0.2.0 LANGUAGES C CXX ASM_MASM)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows x64 with MSVC required")
endif()
set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: ")
set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033")
set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;VSLANG=1033")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(detours STATIC IMPORTED)
set_target_properties(detours PROPERTIES
  IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/detours/detours.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Preserve the runtime compiler flags while moving all plugin HLSL compilation
# to configure time. Device-specific PSOs are still created at runtime.
if(NOT FXC)
  file(GLOB LITEDX11_FXC
    "$ENV{WindowsSdkDir}bin/*/x64/fxc.exe"
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
  if(LITEDX11_FXC)
    list(SORT LITEDX11_FXC COMPARE NATURAL ORDER DESCENDING)
    list(GET LITEDX11_FXC 0 FXC)
  endif()
endif()
if(NOT FXC)
  message(FATAL_ERROR "fxc.exe not found; install Windows SDK or configure -DFXC=<path>")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LITEDX11_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/nr_shaders.hpp.in")
file(WRITE "${LITEDX11_SHADER_HEADER}" "#pragma once\n// Generated HLSL bytecode; edit src/nr/shaders and rebuild.\n")
function(litedx11_shader source entry profile symbol)
  set(path "${CMAKE_CURRENT_SOURCE_DIR}/src/nr/shaders/${source}.hlsl")
  set(cso "${CMAKE_CURRENT_BINARY_DIR}/generated/${symbol}.cso")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
  execute_process(COMMAND "${FXC}" /nologo /T "${profile}" /E "${entry}" /O3 ${ARGN} /Fo "${cso}" "${path}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "fxc failed on ${source}:${entry}: ${output}${error}")
  endif()
  file(READ "${cso}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  file(APPEND "${LITEDX11_SHADER_HEADER}" "inline constexpr unsigned char ${symbol}[] = {${bytes}};\n")
endfunction()
litedx11_shader(lite_color Main cs_5_0 liteColorShader)
litedx11_shader(lite_preview Main cs_5_0 litePreviewShader)
litedx11_shader(lite_meter Main cs_5_0 liteMeterShader)
litedx11_shader(bridge_convert Depth cs_5_0 bridgeDepthShader)
litedx11_shader(bridge_convert Motion cs_5_0 bridgeMotionShader)
litedx11_shader(dense_flow FlowVS vs_5_0 flowVertexShader /Ges /Gis)
foreach(stage Prepare DepthMip Down Match Pool Upscale Finish)
  litedx11_shader(dense_flow "PS${stage}" ps_5_0 "flow${stage}Shader" /Ges /Gis)
endforeach()
# COPYONLY preserves the output timestamp when bytecode did not change.
configure_file("${LITEDX11_SHADER_HEADER}" "${CMAKE_CURRENT_BINARY_DIR}/generated/nr_shaders.hpp" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/flow_noise.r8)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/flow_noise.r8" DENSE_FLOW_NOISE HEX)
string(REGEX REPLACE "(..)" "0x\\1," DENSE_FLOW_NOISE "${DENSE_FLOW_NOISE}")
configure_file(src/nr/dense_shader.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/dense_shader.hpp" @ONLY)

add_library(litedx11_frames STATIC
  src/nr/bridge.cpp
  src/nr/preview.cpp
  src/nr/dense_flow.cpp
  src/nr/depth11.cpp
  src/nr/white_point.cpp
  src/nr/pass.cpp)
target_include_directories(litedx11_frames PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(litedx11_frames PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(litedx11_frames PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(litedx11_frames PUBLIC d3d11 d3d12 dxgi.lib d3dcompiler)

add_library(litedx11_runtime SHARED
  src/bootstrap.cpp
  src/runtime.cpp
  src/config_writer.cpp
  src/config_storage.cpp
  src/hooks.cpp
  src/panel.cpp
  src/panel_ui.cpp
  src/panel_gpu.cpp
  src/ui_widgets.cpp
  src/input.cpp
  src/input_keyboard.cpp
  src/input_directinput.cpp
  src/nr/caller.cpp
  src/nr/model_runtime.cpp
  src/nr/model_settings.cpp
  src/nr/capture11.cpp
  src/nr/capture.cpp
  src/nr/depth_hooks.cpp)
set_target_properties(litedx11_runtime PROPERTIES OUTPUT_NAME "nvngx.dll_litedx11")
target_link_libraries(litedx11_runtime PRIVATE
  litedx11_frames detours imgui d3d11 d3d12 dxgi.lib d3dcompiler
  user32 dwmapi dinput8 dxguid advapi32 version)
target_compile_definitions(litedx11_runtime PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN "LITE_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(litedx11_runtime PRIVATE /W4 /EHsc /utf-8)

add_library(dxgi SHARED src/proxy.cpp src/dxgi.asm src/dxgi.def)
target_link_libraries(dxgi PRIVATE user32)
add_dependencies(dxgi litedx11_runtime)
target_compile_definitions(dxgi PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dxgi PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4>
  $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

# Alternative entry names, OptiScaler style: the game keeps its own dxgi.dll and
# this plugin loads as version.dll (and later winmm.dll, ...) instead.
# src/proxy/<name>.def mirrors the system DLL's export names and ordinals; the
# jump stubs and the name table are generated from that file at configure time,
# so adding a name means adding one .def plus one call at the bottom.
set(LITEDX11_ENTRY_TARGETS "")
function(litedx11_entry name)
  set(def "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/${name}.def")
  set(headerDir "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy-${name}")
  file(MAKE_DIRECTORY "${headerDir}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${def}")
  file(STRINGS "${def}" LITEDX11_DEF_LINES)
  set(LITEDX11_ENTRY_NAMES "")
  set(LITEDX11_ENTRY_INDICES "")
  foreach(line IN LISTS LITEDX11_DEF_LINES)
    if(line MATCHES "^[ \t]*([^=# \t]+)=Proxy([0-9]+)[ \t]*@([0-9]+)")
      list(APPEND LITEDX11_ENTRY_NAMES "${CMAKE_MATCH_1}")
      list(APPEND LITEDX11_ENTRY_INDICES "${CMAKE_MATCH_2}")
    endif()
  endforeach()
  list(LENGTH LITEDX11_ENTRY_NAMES count)
  if(count LESS 1)
    message(FATAL_ERROR "No exports parsed from ${def}")
  endif()
  set(slot 0)
  foreach(index IN LISTS LITEDX11_ENTRY_INDICES)
    if(NOT index EQUAL slot)
      message(FATAL_ERROR "${def}: Proxy${index} at position ${slot}; stubs must be numbered in order")
    endif()
    math(EXPR slot "${slot}+1")
  endforeach()
  set(header "// Generated at configure time from src/proxy/${name}.def. Never edit.\n#pragma once\n")
  string(APPEND header "inline constexpr unsigned kProxyExportCount = ${count};\n")
  string(APPEND header "inline constexpr const wchar_t* kProxyTargetName = L\"${name}.dll\";\n")
  string(APPEND header "inline constexpr const wchar_t* kProxyOriginalName = L\"${name}-original.dll\";\n")
  string(APPEND header "inline constexpr const char* kProxyExportNames[${count}] = {\n")
  foreach(export IN LISTS LITEDX11_ENTRY_NAMES)
    string(APPEND header "    \"${export}\",\n")
  endforeach()
  string(APPEND header "};\n")
  file(WRITE "${headerDir}/proxy_exports.generated.hpp" "${header}")

  set(asm "EXTERN ResolveProxy:PROC\nEXTERN ProxyTargets:QWORD\n.code\n")
  string(APPEND asm "FORWARD MACRO number\nLOCAL resolved\nProxy&number PROC\n")
  string(APPEND asm " mov rax, QWORD PTR [ProxyTargets + number * 8]\n test rax, rax\n jnz resolved\n")
  string(APPEND asm " jmp Slow&number\nresolved:\n jmp rax\nProxy&number ENDP\nSlow&number PROC FRAME\n")
  string(APPEND asm " push rcx\n .allocstack 8\n push rdx\n .allocstack 8\n")
  string(APPEND asm " push r8\n .allocstack 8\n push r9\n .allocstack 8\n sub rsp, 104\n .allocstack 104\n")
  string(APPEND asm " .endprolog\n movdqu [rsp+32], xmm0\n movdqu [rsp+48], xmm1\n movdqu [rsp+64], xmm2\n")
  string(APPEND asm " movdqu [rsp+80], xmm3\n mov ecx, number\n call ResolveProxy\n mov r11, rax\n")
  string(APPEND asm " movdqu xmm0, [rsp+32]\n movdqu xmm1, [rsp+48]\n movdqu xmm2, [rsp+64]\n movdqu xmm3, [rsp+80]\n")
  string(APPEND asm " add rsp, 104\n pop r9\n pop r8\n pop rdx\n pop rcx\n jmp r11\nSlow&number ENDP\nENDM\n")
  math(EXPR last "${count}-1")
  foreach(index RANGE ${last})
    string(APPEND asm "FORWARD ${index}\n")
  endforeach()
  string(APPEND asm "END\n")
  file(WRITE "${headerDir}/proxy_${name}.asm" "${asm}")

  add_library(entry_${name} SHARED src/proxy_forward.cpp "${headerDir}/proxy_${name}.asm" "${def}")
  set_target_properties(entry_${name} PROPERTIES OUTPUT_NAME "${name}")
  target_include_directories(entry_${name} PRIVATE "${headerDir}")
  target_link_libraries(entry_${name} PRIVATE user32)
  add_dependencies(entry_${name} litedx11_runtime)
  target_compile_definitions(entry_${name} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(entry_${name} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  set(LITEDX11_ENTRY_TARGETS ${LITEDX11_ENTRY_TARGETS} entry_${name} PARENT_SCOPE)
endfunction()
litedx11_entry(version)
litedx11_entry(winmm)

configure_file(DLSSNR_LiteDX11.ini DLSSNR_LiteDX11.ini COPYONLY)
install(TARGETS dxgi litedx11_runtime ${LITEDX11_ENTRY_TARGETS} RUNTIME DESTINATION .)
install(FILES DLSSNR_LiteDX11.ini third_party/NOTICES.txt LICENSE DESTINATION .)
install(FILES Licenses/RenoDX_ATTRIBUTION.txt DESTINATION Licenses)
