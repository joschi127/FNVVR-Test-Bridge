// Loads an OpenXR loader DLL and asks it for the instance extensions: shows whether the loader accepts the runtime that
// XR_RUNTIME_JSON names. Set XR_LOADER_DEBUG=all for the loader's own messages.
// Usage: loader_probe <path to openxr_loader.dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <cstdio>

int main(int argc, char** argv) {
  HMODULE loader = LoadLibraryA(argc > 1 ? argv[1] : "openxr_loader.dll");
  if (!loader) { std::printf("loader not loaded (%lu)\n", GetLastError()); return 1; }
  auto gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
  if (!gipa) { std::printf("no xrGetInstanceProcAddr\n"); return 1; }
  PFN_xrVoidFunction f = nullptr;
  gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &f);
  auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(f);
  uint32_t count = 0;
  XrResult r = enumerate ? enumerate(nullptr, 0, &count, nullptr) : XR_ERROR_FUNCTION_UNSUPPORTED;
  std::printf("xrEnumerateInstanceExtensionProperties: %d, %u extension(s)\n", int(r), count);
  return XR_SUCCEEDED(r) ? 0 : 1;
}
