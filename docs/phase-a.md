# Phase A: direct rendering into four swapchain regions

Scope: four independently sized and positioned colored rectangles, drawn
directly into a single swapchain image. Later phases require explicit approval.
Phase B's offscreen images, Phase C's real content, and Phase D's simulation
frame association are not part of this implementation.

## Layout

`XVS::displayLayout()` defines the cockpit arrangement. Coordinates are
fractions of framebuffer width/height, with the origin at the upper left.
These are the existing provisional proportions, not measured NASA dimensions.
The development plan refers to a cockpit reference image, but that image was
not supplied; exact correspondence to that reference remains unverified.

| Display | X | Y | Width | Height | Color |
| --- | ---: | ---: | ---: | ---: | --- |
| Standby | 0.04 | 0.28 | 0.16 | 0.18 | Amber |
| External vision | 0.23 | 0.05 | 0.65 | 0.43 | Blue |
| Navigation | 0.08 | 0.56 | 0.40 | 0.36 | Green |
| Primary flight | 0.52 | 0.56 | 0.40 | 0.36 | Purple |

`main.cpp` converts this layout into generic `RenderRegion` values. The engine
does not need to know cockpit names or the UDP protocol.

## How the drawing works

One graphics pipeline uses a vertex shader to generate a triangle large enough
to cover the current viewport, without a vertex buffer. For each display, the
dynamic **viewport** maps that triangle into the display's rectangle. The
**scissor** clips fragments to its pixel boundaries. A four-float **push
constant** supplies the region's color to the fragment shader.

Each frame acquires one swapchain image, transitions it to color-attachment
layout with synchronization2, and begins one dynamic-rendering operation.
The clear fills the background; four draw commands fill the regions. The
engine then transitions that same image to presentation layout, submits one
command buffer, and calls `presentKHR` once. There are no offscreen images.

Normalized rectangle edges are rounded to framebuffer pixels on every draw.
On resize the engine waits for device work, rebuilds swapchain resources and
the format-dependent pipeline, then draws with the new extent. A minimized
window waits for events instead of creating a zero-sized swapchain. Frame
fences protect command-buffer reuse, and render-finished semaphores are indexed
by swapchain image.

## Fixes

- `acquireNextImage` returns `vk::ResultValue<uint32_t>` in the configured SDK;
  use `.result` and `.value` instead of pair fields `.first` and `.second`.
- The RAII `pushConstants<float>` overload accepts a typed array and infers
  its byte size. Pass the four-element color array rather than the five-argument
  raw Vulkan form.
- `compile.bat` now invokes the CMake shader target and propagates its exit
  status instead of reporting success without compiling shaders.
- A Debug-only debug-utils messenger records validation warnings/errors,
  including during instance creation, to stderr.

## Verification

Run the commands in the README to reproduce these checks. The verification
script captures only the window of the XVS process it starts; it samples pixels
throughout the four rectangles and background, allowing for edge rounding and
sRGB conversion. It does not access the Unreal project.

| Requirement | Evidence |
| --- | --- |
| Four independently defined regions | `RenderRegion`, `XVSDisplayRegion`, and the layout above |
| Graphics pipeline, dynamic viewport/scissor, distinct colors | Debug build plus pixel checks across all four regions |
| Resolution-independent positioning | Captures at 1200x800, 955x704, 1501x908, and 640x482 framebuffer sizes on the verification machine |
| Resize, minimize, restore, and exit | Automated window operations, restored-frame pixel checks, and successful exit while minimized |
| One swapchain and one present per frame | `Engine::drawFrame` has one acquisition, one rendering operation with four draws, and one `presentKHR` call |
| Existing UDP reception | Two nine-field CSV datagrams to localhost:5500; 80 and 240 knots change background blue while display colors stay fixed |
| Vulkan diagnostics | Validation enabled in Debug; no errors or VUID diagnostics during the verification run |

The test uses synthetic UDP packets in the existing format, not a live Unreal
session. `build/verification/application.stderr.log` also records environment
warnings: on this machine, three Galaxy overlay layer-name warnings and an
OBS layer API-version warning were reported. No overlay settings were changed.
