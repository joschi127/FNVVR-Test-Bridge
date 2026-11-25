// Smoke test of the FNVVR null OpenXR runtime without a graphics device: negotiation, instance, system, paths,
// spaces and views, events. The session and frame loop need a Vulkan device and are tested in the game (run c51 ff.).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failed = 0;

#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "fnvvr_null_xr.dll";
  HMODULE dll = LoadLibraryA(path);
  CHECK(dll != nullptr);

  if (!dll)
    return 1;

  auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(GetProcAddress(dll, "xrNegotiateLoaderRuntimeInterface"));
  CHECK(negotiate != nullptr);

  if (!negotiate)
    return 1;

  XrNegotiateLoaderInfo info = { XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo) };
  info.minInterfaceVersion = 1;
  info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
  info.maxApiVersion = XR_MAKE_VERSION(1, 1, 0x3ff);

  XrNegotiateRuntimeRequest request = { XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest) };
  CHECK(negotiate(&info, &request) == XR_SUCCESS);
  CHECK(XR_VERSION_MAJOR(request.runtimeApiVersion) == 1 && XR_VERSION_MINOR(request.runtimeApiVersion) == 0);
  CHECK(request.getInstanceProcAddr != nullptr);

  PFN_xrGetInstanceProcAddr gipa = request.getInstanceProcAddr;

  auto get = [&] (XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    gipa(instance, name, &f);
    return f;
  };

  PFN_xrVoidFunction missing = nullptr;
  CHECK(gipa(XR_NULL_HANDLE, "xrNoSuchFunction", &missing) == XR_ERROR_FUNCTION_UNSUPPORTED && !missing);

  auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(get(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties"));
  uint32_t count = 0;
  CHECK(enumerate(nullptr, 0, &count, nullptr) == XR_SUCCESS && count == 1);
  XrExtensionProperties ext = { XR_TYPE_EXTENSION_PROPERTIES };
  CHECK(enumerate(nullptr, 1, &count, &ext) == XR_SUCCESS && !std::strcmp(ext.extensionName, "XR_KHR_vulkan_enable"));

  auto create = reinterpret_cast<PFN_xrCreateInstance>(get(XR_NULL_HANDLE, "xrCreateInstance"));
  XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
  std::strcpy(ci.applicationInfo.applicationName, "null_runtime_test");
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  const char* unknown[] = { "XR_EXT_local_floor" };
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = unknown;
  XrInstance instance = XR_NULL_HANDLE;
  CHECK(create(&ci, &instance) == XR_ERROR_EXTENSION_NOT_PRESENT);

  const char* vulkan[] = { "XR_KHR_vulkan_enable" };
  ci.enabledExtensionNames = vulkan;
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
  CHECK(create(&ci, &instance) == XR_ERROR_API_VERSION_UNSUPPORTED);  // only 1.0 is implemented
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  CHECK(create(&ci, &instance) == XR_SUCCESS && instance != XR_NULL_HANDLE);

  auto getSystem = reinterpret_cast<PFN_xrGetSystem>(get(instance, "xrGetSystem"));
  XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  CHECK(getSystem(instance, &sgi, &system) == XR_SUCCESS && system != XR_NULL_SYSTEM_ID);

  auto properties = reinterpret_cast<PFN_xrGetSystemProperties>(get(instance, "xrGetSystemProperties"));
  XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
  CHECK(properties(instance, system, &sp) == XR_SUCCESS && sp.graphicsProperties.maxLayerCount >= 1);

  auto views = reinterpret_cast<PFN_xrEnumerateViewConfigurationViews>(get(instance, "xrEnumerateViewConfigurationViews"));
  XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
  CHECK(views(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, vcv) == XR_SUCCESS && count == 2
    && vcv[0].recommendedImageRectWidth > 0);

  auto toPath = reinterpret_cast<PFN_xrStringToPath>(get(instance, "xrStringToPath"));
  auto toString = reinterpret_cast<PFN_xrPathToString>(get(instance, "xrPathToString"));
  XrPath left = XR_NULL_PATH, again = XR_NULL_PATH;
  CHECK(toPath(instance, "/user/hand/left", &left) == XR_SUCCESS && left != XR_NULL_PATH);
  CHECK(toPath(instance, "/user/hand/left", &again) == XR_SUCCESS && again == left);
  char text[64] = { };
  CHECK(toString(instance, left, sizeof(text), &count, text) == XR_SUCCESS && !std::strcmp(text, "/user/hand/left"));

  // Spaces work without a session in this runtime: the view space sits at eye height above the stage
  auto createSpace = reinterpret_cast<PFN_xrCreateReferenceSpace>(get(instance, "xrCreateReferenceSpace"));
  auto locate = reinterpret_cast<PFN_xrLocateSpace>(get(instance, "xrLocateSpace"));
  XrReferenceSpaceCreateInfo rsi = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
  rsi.poseInReferenceSpace.orientation.w = 1.0f;
  XrSpace stage = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  CHECK(createSpace(XR_NULL_HANDLE, &rsi, &stage) == XR_SUCCESS);
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  CHECK(createSpace(XR_NULL_HANDLE, &rsi, &view) == XR_SUCCESS);
  XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
  CHECK(locate(view, stage, 0, &location) == XR_SUCCESS && std::fabs(location.pose.position.y - 1.65f) < 1e-4f);
  CHECK(locate(stage, view, 0, &location) == XR_SUCCESS && std::fabs(location.pose.position.y + 1.65f) < 1e-4f);

  auto locateViews = reinterpret_cast<PFN_xrLocateViews>(get(instance, "xrLocateViews"));
  XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
  vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  vli.space = stage;
  XrViewState vs = { XR_TYPE_VIEW_STATE };
  XrView eye[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
  CHECK(locateViews(XR_NULL_HANDLE, &vli, &vs, 2, &count, eye) == XR_SUCCESS && count == 2
    && eye[0].pose.position.x < eye[1].pose.position.x && eye[0].fov.angleLeft < 0.0f);

  auto destroySpace = reinterpret_cast<PFN_xrDestroySpace>(get(instance, "xrDestroySpace"));
  CHECK(destroySpace(stage) == XR_SUCCESS && destroySpace(view) == XR_SUCCESS);
  CHECK(destroySpace(stage) == XR_ERROR_HANDLE_INVALID);                  // destroyed handles are caught
  CHECK(locate(view, stage, 0, &location) == XR_ERROR_HANDLE_INVALID);

  auto resultName = reinterpret_cast<PFN_xrResultToString>(get(instance, "xrResultToString"));
  char name[XR_MAX_RESULT_STRING_SIZE] = { };
  CHECK(resultName(instance, XR_ERROR_LAYER_INVALID, name) == XR_SUCCESS && !std::strcmp(name, "XR_ERROR_LAYER_INVALID"));

  auto poll = reinterpret_cast<PFN_xrPollEvent>(get(instance, "xrPollEvent"));
  XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
  CHECK(poll(instance, &event) == XR_EVENT_UNAVAILABLE);

  auto destroy = reinterpret_cast<PFN_xrDestroyInstance>(get(instance, "xrDestroyInstance"));
  CHECK(destroy(instance) == XR_SUCCESS);

  std::printf(g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed);
  return g_failed ? 1 : 0;
}
