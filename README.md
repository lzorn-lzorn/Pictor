# Hello Vulkan

一个基于 **Vulkan 1.0** 的最小示例：使用 `external/SDL`（SDL3）创建窗口和
Vulkan surface，绘制一个彩色三角形。

## 依赖

- **Vulkan 头文件**：由 CMake `FetchContent` 自动拉取 `KhronosGroup/Vulkan-Headers`
  （锁定 tag `vulkan-sdk-1.4.350.0`），无需安装 Vulkan SDK。
- **DXC**（DirectX Shader Compiler）：由 CMake 自动下载预编译包（锁定
  `v1.9.2609` + SHA256），无需安装。
- SDL3：已随仓库放在 `external/SDL`，无需额外安装。
- CMake ≥ 3.28 与一个 C++23 编译器（MSVC 19.34+ / VS 2022 17.4+，或同等级的 Clang/GCC）。
- 一个支持 Vulkan 的 GPU 及其驱动（运行期由 `vk::raii` 动态加载 `vulkan-1.dll`，来自显卡驱动）。

> 首次 configure 需要联网拉取 Vulkan-Headers 和 DXC（下载到 `build/_deps/` 与
> `build/_dxc/`），之后会被缓存，可离线重建。

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

也可以用 Ninja + MSVC 工具链（在 `vcvars64.bat` 环境中），或直接用 CLion
打开仓库根目录让 CMake 自动识别生成器。

## 运行

```powershell
.\build\bin\HelloVulkan.exe
```

构建过程会把 `SDL3.dll` 拷贝到可执行文件旁边。着色器用 **HLSL** 编写，在构建时
由 **DXC**（`-spirv -T vs_6_0 / ps_6_0`）编译成 SPIR-V，再内嵌为 C++ 头文件
（`generated/shaders/*_spv.h`），因此运行时不需要额外的着色器文件。

## 结构

```
CMakeLists.txt                    # 根构建脚本：接线模块与可执行文件
cmake/spv_to_header.cmake         # 将 SPIR-V 二进制转为 C++ 头文件
shaders/triangle.vert.hlsl        # 顶点着色器（HLSL，DXC 编译为 SPIR-V）
shaders/triangle.frag.hlsl        # 片元着色器（HLSL，DXC 编译为 SPIR-V）
src/main.cpp                      # 入口：驱动渲染循环
src/infra/                        # Infra 基础模块（仅依赖标准库）
  CMakeLists.txt                  #   Infra INTERFACE 库（header-only）
  containers/ObjectPool.hpp
  tracer/Benchmark.hpp
src/runtime/platform/             # GenericPlatform 平台抽象（header-only）
  IGenericPlatform.h              #   IGenericPlatform 接口
  CMakeLists.txt                  #   GenericPlatform INTERFACE 库
src/runtime/renderer/             # Renderer 渲染模块（自包含 SDL3/Vulkan/DXC/着色器）
  Renderer.h / Renderer.cpp
  CMakeLists.txt                  #   Renderer 静态库（链接 Infra + Rhi）
  rhi/                            # Rhi 库（Vulkan 后端）
    Rhi.h / Rhi.cpp
    CMakeLists.txt                #   Rhi 静态库（链接 Vulkan::Headers / SDL3）
external/SDL                      # 第三方 SDL3 源码
```

## 代码组织与命名规范

- **不使用 C++20 模块**：全部用传统头文件 + `#include`；Vulkan 通过
  `#include <vulkan/vulkan.hpp>` 和 `<vulkan/vulkan_raii.hpp>` 使用 vulkan-hpp
  （`vk::` / `vk::raii`），不使用 C 的 `Vk*` API。
- **模块分层**：`Infra`（标准库基础工具）为独立模块；`Renderer` 是自包含的
  渲染模块（内部含 `Rhi` Vulkan 后端与 SDL3 窗口）；`GenericPlatform` 提供
  平台接口。应用层只 `#include "Renderer.h"`，不接触 SDL 或 Vulkan。
- 命名规范：
  - 函数名：动词开头 + 小驼峰（`drawFrame`、`createInstance`、`getFrameCount`）。
  - bool 变量：一般疑问词 + 大驼峰（`IsQuit`、`IsFramebufferResized`、`AreExtensionsSupported`）。
  - 类名与变量名：大驼峰（`Renderer`、`Rhi`、`QueueFamilyIndices`、`Window`）。
  - 接口：`I` 前缀（`IGenericPlatform`）。
  - 枚举：`E` 前缀。

## 关于 Vulkan 1.0

- `vk::ApplicationInfo::apiVersion` 设为 `vk::ApiVersion10`（即 1.0）。
- 交换链通过 1.0 时代的 `VK_KHR_swapchain` 设备扩展获取。
- 未使用任何 1.1+ 的 API 或功能位，仅依赖 SDL 返回的 `VK_KHR_surface` /
  平台 surface 实例扩展。
