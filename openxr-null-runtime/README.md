# FNVVR null OpenXR runtime

A small OpenXR runtime without a headset, for automated tests of FNVVR's OpenXR backend
(`d3d9.vrBackend = openxr`). It lets the game run a real OpenXR session on a PC with no headset and no
VR software: FNVVR's OpenXR code paths (instance, session, swapchains, frame loop, panels as quad layers,
input actions) run as they would with a headset. Nothing is shown; what a test needs to see or drive goes
through FNVVR's [VR test channel](https://github.com/joschi127/FNVVR/blob/coop-compat/docs/testing/vr-test-channel.md)
(head and hand poses, actions, captures of the eyes and panels).

Why a runtime of our own: SteamVR's OpenXR runtime with its null driver cannot name a Vulkan device
(`xrGetVulkanGraphicsDeviceKHR` fails, the null driver has no output device), and the other headset-free
runtimes (Monado, Meta XR Simulator) are not available for 32-bit Windows programs.

## What it does

- Instance with `XR_KHR_vulkan_enable` only; one head-mounted system with two views.
- Reference spaces VIEW, LOCAL, STAGE; a standing player (eyes 1.65 m above the floor, 64 mm apart),
  hands in front at waist height; poses never move (the test channel sets the poses a test needs).
- Actions are active and idle (nothing pressed); the first suggested interaction profile is reported as bound.
- Swapchains are Vulkan images on the application's device (three per swapchain, device-local memory).
- `xrWaitFrame` paces frames at the set rate, or not at all (`FNVVR_NULL_XR_RATE=0`) for performance runs.
- `xrEndFrame` checks the layers and refuses frames with more than `maxLayerCount` layers, as a real runtime does.

Not supported: other graphics APIs, other extensions, eye tracking, hand tracking, haptics (accepted and ignored).

## Settings (environment variables of the game's process)

| Variable | Default | Meaning |
|---|---|---|
| `FNVVR_NULL_XR_SIZE` | `2292x1884` | Recommended eye image size |
| `FNVVR_NULL_XR_RATE` | `90` | Display rate in Hz; `0` runs unpaced |
| `FNVVR_NULL_XR_LAYERS` | `16` | `maxLayerCount` reported and enforced (the spec's minimum) |
| `FNVVR_NULL_XR_LOG` | (none) | File the runtime appends a short log to: load, session, swapchains, frames per second and layers every 10 s, refused frames |

## Use

Build with the `x86` preset (`cmake --preset x86`, `cmake --build --preset release`); the output is
`build/Release/fnvvr_null_xr.dll` with its manifest `fnvvr_null_xr.json`. Point the OpenXR loader of the
game's process at the manifest, for that process only:

```
set XR_RUNTIME_JSON=C:\path\to\FNVVR-Test-Bridge\build\Release\fnvvr_null_xr.json
```

then start `FalloutNV.exe` from that environment with `d3d9.vrBackend = openxr` and FNVVR's
`fnvvr\openxr_loader.dll` in place. The registry's active runtime is not touched. The manifest names the
DLL as `./fnvvr_null_xr.dll`: a bare file name would make the loader search the system path instead of the
manifest's folder.

The runtime needs no screen: it uses only the graphics device and a timer, and FNVVR hides the game's window
in VR. Runs work with the monitor switched off or showing another input; what a test needs to see comes from
the VR test channel's captures. A PC with no display at all reported by Windows is untested.

`loader_probe <openxr_loader.dll>` shows whether a loader accepts the runtime that `XR_RUNTIME_JSON` names
(set `XR_LOADER_DEBUG=all` for the loader's own messages). `null_runtime_test` (ctest) checks the runtime
without a graphics device.

## Licence

zlib, as the rest of this repository. The Khronos headers in `external/openxr` and `external/vulkan` are
Apache-2.0 OR MIT (licence files there).
