# X-59 XVS Vulkan

Phase A renders the four cockpit display regions directly into one Vulkan
swapchain. UDP telemetry from Unreal/JSBSim continues to arrive on port 5500.
Each display currently shows a distinct test color; received airspeed changes
the blue background between displays.

## Build and run

Requires Windows, MSVC 2022, CMake 3.24+, the Vulkan 1.4 SDK (including `glslc`
and the Khronos validation layer), and a Vulkan 1.4 capable GPU/driver.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel 4
.\build\Debug\VulkanTutorial.exe
```

CMake builds both region shaders automatically into `build/shaders`.
`compile.bat` builds just the shader target in an already configured project.
Debug builds log Vulkan validation warnings and errors to stderr.

## Verify Phase A

After the Debug build, close other XVS instances and pause external telemetry
senders so the test can use port 5500 and control its input deterministically:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify-phase-a.ps1
```

The script starts its own XVS process, checks rendered pixels at four window
sizes, minimizes/restores the window, sends two loopback telemetry packets,
and verifies shutdown while minimized. It controls and captures only that
process's window. Captures, application logs, and `results.txt` are saved in
`build/verification`. Application errors and Vulkan VUID diagnostics fail the
test; other Vulkan warnings are retained in the report for inspection.

See [Phase A implementation and verification](docs/phase-a.md) for the layout,
rendering explanation, and acceptance checks. Advance to Phase B only when
explicitly requested.
