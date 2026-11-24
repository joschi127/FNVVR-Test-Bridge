// FNVVR null OpenXR runtime: a headset-free OpenXR runtime for automated tests of FNVVR's OpenXR backend.
//
// It does just enough for FNVVR (d3d9.vrBackend = openxr) to run a session: an instance with XR_KHR_vulkan_enable,
// one stereo HMD system, reference and action spaces with fixed poses, actions that read as idle, swapchains made
// of Vulkan images on the application's device, and a frame loop paced like a headset. Nothing is shown; the
// frames' images are only counted. What a test sees and drives comes through FNVVR's VR test channel (head and
// hand poses, actions, captures). See README.md.
//
// Settings (environment variables of the game's process, all optional):
//   FNVVR_NULL_XR_SIZE    eye image size, "2292x1884" by default
//   FNVVR_NULL_XR_RATE    display rate in Hz, 90 by default; 0 runs unpaced (as fast as the game goes)
//   FNVVR_NULL_XR_LAYERS  maxLayerCount reported and enforced, 16 by default (the spec's minimum)
//   FNVVR_NULL_XR_LOG     file to append a short log to (start, session, frames every 10 s, refused frames)
//
// zlib licence, see LICENSE in the repository root.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

  // ---------------------------------------------------------------- settings and log

  struct Settings {
    uint32_t width  = 2292;
    uint32_t height = 1884;
    double   rate   = 90.0;
    uint32_t layers = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    std::string log;
  };

  std::string env(const char* name) {
    char buffer[1024] = { };
    DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return n && n < sizeof(buffer) ? std::string(buffer, n) : std::string();
  }

  Settings readSettings() {
    Settings s;
    std::string size = env("FNVVR_NULL_XR_SIZE");
    unsigned w = 0, h = 0;

    if (std::sscanf(size.c_str(), "%ux%u", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 8192 && h <= 8192) {
      s.width  = w;
      s.height = h;
    }

    std::string rate = env("FNVVR_NULL_XR_RATE");

    if (!rate.empty())
      s.rate = std::clamp(std::atof(rate.c_str()), 0.0, 1000.0);

    std::string layers = env("FNVVR_NULL_XR_LAYERS");

    if (!layers.empty())
      s.layers = uint32_t(std::clamp(std::atoi(layers.c_str()), 1, 64));

    s.log = env("FNVVR_NULL_XR_LOG");
    return s;
  }

  const Settings& settings() {
    static const Settings s = readSettings();
    return s;
  }

  std::mutex g_logMutex;

  void logLine(const char* format, ...) {
    if (settings().log.empty())
      return;

    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);

    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = nullptr;

    if (fopen_s(&f, settings().log.c_str(), "a") == 0 && f) {
      std::fprintf(f, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
      std::fclose(f);
    }
  }

  // ---------------------------------------------------------------- time

  int64_t nowNs() {
    static const int64_t frequency = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return f.QuadPart;
    }();

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(double(c.QuadPart) * 1e9 / double(frequency));
  }

  void sleepUntilNs(int64_t when) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    int64_t left = when - nowNs();

    if (left <= 0)
      return;

    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -(left / 100);  // relative, in 100 ns units

      if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    } else {
      Sleep(DWORD(left / 1000000));
    }

    while (nowNs() < when)
      YieldProcessor();
  }

  // ---------------------------------------------------------------- pose math

  struct Quat { float x = 0, y = 0, z = 0, w = 1; };
  struct Vec3 { float x = 0, y = 0, z = 0; };
  struct Pose { Quat q; Vec3 p; };

  Quat mul(const Quat& a, const Quat& b) {
    return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
  }

  Quat conj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

  Vec3 rotate(const Quat& q, const Vec3& v) {
    Quat r = mul(mul(q, { v.x, v.y, v.z, 0 }), conj(q));
    return { r.x, r.y, r.z };
  }

  // a then b: b expressed in a's frame
  Pose compose(const Pose& a, const Pose& b) {
    Vec3 p = rotate(a.q, b.p);
    return { mul(a.q, b.q), { a.p.x + p.x, a.p.y + p.y, a.p.z + p.z } };
  }

  Pose inverse(const Pose& a) {
    Quat q = conj(a.q);
    Vec3 p = rotate(q, a.p);
    return { q, { -p.x, -p.y, -p.z } };
  }

  Pose fromXr(const XrPosef& x) {
    Pose p;
    p.q = { x.orientation.x, x.orientation.y, x.orientation.z, x.orientation.w };
    p.p = { x.position.x, x.position.y, x.position.z };

    float n = std::sqrt(p.q.x * p.q.x + p.q.y * p.q.y + p.q.z * p.q.z + p.q.w * p.q.w);

    if (n > 0.0f) {
      p.q.x /= n; p.q.y /= n; p.q.z /= n; p.q.w /= n;
    } else {
      p.q = Quat();
    }
    return p;
  }

  XrPosef toXr(const Pose& p) {
    XrPosef x;
    x.orientation = { p.q.x, p.q.y, p.q.z, p.q.w };
    x.position    = { p.p.x, p.p.y, p.p.z };
    return x;
  }

  // Where things are in the stage (floor at y = 0, -z forward): a standing player, hands in front at the waist
  constexpr float HeadHeight = 1.65f;
  constexpr float EyeOffset  = 0.032f;   // half of a 64 mm IPD
  const Pose HeadPose  = { {}, { 0.0f, HeadHeight, 0.0f } };
  const Pose LeftHand  = { {}, { -0.2f, 1.1f, -0.3f } };
  const Pose RightHand = { {}, {  0.2f, 1.1f, -0.3f } };
  const XrFovf EyeFov  = { -0.80f, 0.80f, 0.85f, -0.90f };

  // ---------------------------------------------------------------- objects

  struct Instance;
  struct Session;

  enum class SpaceKind { Reference, Action };

  struct Space {
    Session*              session   = nullptr;
    SpaceKind             kind      = SpaceKind::Reference;
    XrReferenceSpaceType  reference = XR_REFERENCE_SPACE_TYPE_STAGE;
    int                   hand      = -1;  // 0 left, 1 right, for action spaces
    Pose                  offset;
  };

  struct SwapchainImage {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
  };

  struct Swapchain {
    Session*                    session  = nullptr;
    uint32_t                    width    = 0;
    uint32_t                    height   = 0;
    int64_t                     format   = 0;
    std::vector<SwapchainImage> images;
    uint32_t                    next     = 0;
    std::deque<uint32_t>        acquired;
    bool                        waited   = false;
  };

  struct ActionSet { std::string name; };

  struct Action {
    ActionSet*   set  = nullptr;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::string  name;
  };

  struct Vulkan {
    HMODULE                                   library            = nullptr;
    PFN_vkGetInstanceProcAddr                 getInstanceProcAddr = nullptr;
    VkInstance                                instance           = VK_NULL_HANDLE;
    VkPhysicalDevice                          physical           = VK_NULL_HANDLE;
    VkDevice                                  device             = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceMemoryProperties   memoryProperties   = nullptr;
    PFN_vkCreateImage                         createImage        = nullptr;
    PFN_vkDestroyImage                        destroyImage       = nullptr;
    PFN_vkGetImageMemoryRequirements          imageRequirements  = nullptr;
    PFN_vkAllocateMemory                      allocateMemory     = nullptr;
    PFN_vkFreeMemory                          freeMemory         = nullptr;
    PFN_vkBindImageMemory                     bindImageMemory    = nullptr;
    VkPhysicalDeviceMemoryProperties          memory             = { };
  };

  struct Session {
    Instance*       instance = nullptr;
    Vulkan          vk;
    XrSessionState  state    = XR_SESSION_STATE_UNKNOWN;
    bool            running  = false;
    // Frame loop
    int64_t         nextWake = 0;
    int64_t         period   = 0;
    int64_t         display  = 0;
    bool            waited   = false;
    bool            begun    = false;
    // Statistics over the log window
    int64_t         windowStart   = 0;
    uint64_t        frames        = 0;
    uint64_t        framesTotal   = 0;
    uint64_t        layerSum      = 0;
    uint32_t        layerMax      = 0;
    uint64_t        refused       = 0;
    uint64_t        noProjection  = 0;
  };

  struct Instance {
    std::vector<std::string>              paths;    // XrPath = index + 1
    std::deque<XrEventDataBuffer>         events;
    std::vector<std::unique_ptr<ActionSet>> actionSets;
    std::vector<std::unique_ptr<Action>>  actions;
    XrPath                                profile   = XR_NULL_PATH;
    Session*                              session   = nullptr;
  };

  // One lock for the runtime's state; xrWaitFrame does not hold it while it sleeps
  std::recursive_mutex g_mutex;
  Instance*            g_instance = nullptr;

  template<typename T> uint64_t toHandle(T* object) { return uint64_t(uintptr_t(object)); }
  template<typename T> T* fromHandle(uint64_t handle) { return reinterpret_cast<T*>(uintptr_t(handle)); }

  void pushEvent(Instance* instance, const void* event, size_t size) {
    XrEventDataBuffer buffer = { XR_TYPE_EVENT_DATA_BUFFER };
    std::memcpy(&buffer, event, std::min(size, sizeof(buffer)));
    instance->events.push_back(buffer);
  }

  void setState(Session* session, XrSessionState state) {
    session->state = state;
    XrEventDataSessionStateChanged e = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
    e.session = XrSession(toHandle(session));
    e.state   = state;
    e.time    = nowNs();
    pushEvent(session->instance, &e, sizeof(e));
  }

  XrPath stringToPath(Instance* instance, const char* text) {
    for (size_t i = 0; i < instance->paths.size(); i++) {
      if (instance->paths[i] == text)
        return XrPath(i + 1);
    }

    instance->paths.emplace_back(text);
    return XrPath(instance->paths.size());
  }

  const std::string* pathString(Instance* instance, XrPath path) {
    return path && path <= instance->paths.size() ? &instance->paths[size_t(path - 1)] : nullptr;
  }

  int handOf(Instance* instance, XrPath path) {
    const std::string* s = pathString(instance, path);

    if (!s)
      return -1;
    if (*s == "/user/hand/left")
      return 0;
    if (*s == "/user/hand/right")
      return 1;
    return -1;
  }

  // Pose of a space in the stage
  Pose spacePose(const Space* space) {
    Pose base;

    if (space->kind == SpaceKind::Reference) {
      if (space->reference == XR_REFERENCE_SPACE_TYPE_VIEW)
        base = HeadPose;
    } else {
      base = space->hand == 0 ? LeftHand : space->hand == 1 ? RightHand : HeadPose;
    }

    return compose(base, space->offset);
  }

  template<typename T>
  XrResult copyOut(uint32_t capacity, uint32_t* count, T* out, const T* source, uint32_t n) {
    if (!count)
      return XR_ERROR_VALIDATION_FAILURE;

    *count = n;

    if (!capacity)
      return XR_SUCCESS;

    if (capacity < n)
      return XR_ERROR_SIZE_INSUFFICIENT;

    for (uint32_t i = 0; i < n; i++)
      out[i] = source[i];

    return XR_SUCCESS;
  }

  XrResult copyString(uint32_t capacity, uint32_t* count, char* out, const std::string& text) {
    if (!count)
      return XR_ERROR_VALIDATION_FAILURE;

    *count = uint32_t(text.size() + 1);

    if (!capacity)
      return XR_SUCCESS;

    if (capacity < *count)
      return XR_ERROR_SIZE_INSUFFICIENT;

    std::memcpy(out, text.c_str(), text.size() + 1);
    return XR_SUCCESS;
  }

  constexpr XrSystemId SystemId = 1;

  // ---------------------------------------------------------------- instance

  XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName,
      uint32_t capacity, uint32_t* count, XrExtensionProperties* properties) {
    if (layerName)
      return XR_ERROR_API_LAYER_NOT_PRESENT;

    XrExtensionProperties vulkan = { XR_TYPE_EXTENSION_PROPERTIES };
    std::strcpy(vulkan.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    vulkan.extensionVersion = XR_KHR_vulkan_enable_SPEC_VERSION;

    if (!count)
      return XR_ERROR_VALIDATION_FAILURE;

    *count = 1;

    if (!capacity)
      return XR_SUCCESS;

    properties[0].extensionVersion = vulkan.extensionVersion;
    std::strcpy(properties[0].extensionName, vulkan.extensionName);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* out) {
    if (!info || !out)
      return XR_ERROR_VALIDATION_FAILURE;

    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
      if (std::strcmp(info->enabledExtensionNames[i], XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
        return XR_ERROR_EXTENSION_NOT_PRESENT;
    }

    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (g_instance)
      return XR_ERROR_LIMIT_REACHED;

    g_instance = new Instance();
    *out = XrInstance(toHandle(g_instance));

    const Settings& s = settings();
    logLine("instance for '%s': eyes %ux%u, %.1f Hz%s, max layers %u", info->applicationInfo.applicationName,
      s.width, s.height, s.rate, s.rate > 0.0 ? "" : " (unpaced)", s.layers);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || i != g_instance)
      return XR_ERROR_HANDLE_INVALID;

    delete i;
    g_instance = nullptr;
    logLine("instance destroyed");
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    if (!properties)
      return XR_ERROR_VALIDATION_FAILURE;

    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::strcpy(properties->runtimeName, "FNVVR null runtime");
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    const char* name = nullptr;

    switch (value) {
      case XR_SUCCESS:                       name = "XR_SUCCESS"; break;
      case XR_TIMEOUT_EXPIRED:               name = "XR_TIMEOUT_EXPIRED"; break;
      case XR_EVENT_UNAVAILABLE:             name = "XR_EVENT_UNAVAILABLE"; break;
      case XR_SESSION_NOT_FOCUSED:           name = "XR_SESSION_NOT_FOCUSED"; break;
      case XR_FRAME_DISCARDED:               name = "XR_FRAME_DISCARDED"; break;
      case XR_ERROR_VALIDATION_FAILURE:      name = "XR_ERROR_VALIDATION_FAILURE"; break;
      case XR_ERROR_RUNTIME_FAILURE:         name = "XR_ERROR_RUNTIME_FAILURE"; break;
      case XR_ERROR_HANDLE_INVALID:          name = "XR_ERROR_HANDLE_INVALID"; break;
      case XR_ERROR_SIZE_INSUFFICIENT:       name = "XR_ERROR_SIZE_INSUFFICIENT"; break;
      case XR_ERROR_LAYER_LIMIT_EXCEEDED:    name = "XR_ERROR_LAYER_LIMIT_EXCEEDED"; break;
      case XR_ERROR_CALL_ORDER_INVALID:      name = "XR_ERROR_CALL_ORDER_INVALID"; break;
      case XR_ERROR_SESSION_NOT_RUNNING:     name = "XR_ERROR_SESSION_NOT_RUNNING"; break;
      case XR_ERROR_SESSION_RUNNING:         name = "XR_ERROR_SESSION_RUNNING"; break;
      case XR_ERROR_SESSION_NOT_READY:       name = "XR_ERROR_SESSION_NOT_READY"; break;
      case XR_ERROR_FUNCTION_UNSUPPORTED:    name = "XR_ERROR_FUNCTION_UNSUPPORTED"; break;
      case XR_ERROR_EXTENSION_NOT_PRESENT:   name = "XR_ERROR_EXTENSION_NOT_PRESENT"; break;
      case XR_ERROR_GRAPHICS_DEVICE_INVALID: name = "XR_ERROR_GRAPHICS_DEVICE_INVALID"; break;
      case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: name = "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED"; break;
      case XR_ERROR_PATH_INVALID:            name = "XR_ERROR_PATH_INVALID"; break;
      default: break;
    }

    if (name)
      std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", name);
    else
      std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s_%d", XR_SUCCEEDED(value) ? "XR_UNKNOWN_SUCCESS" : "XR_UNKNOWN_FAILURE", int(value));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* event) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || i != g_instance || !event)
      return XR_ERROR_HANDLE_INVALID;

    if (i->events.empty())
      return XR_EVENT_UNAVAILABLE;

    *event = i->events.front();
    i->events.pop_front();
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* text, XrPath* path) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || !text || !path)
      return XR_ERROR_VALIDATION_FAILURE;

    if (text[0] != '/')
      return XR_ERROR_PATH_FORMAT_INVALID;

    *path = stringToPath(i, text);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t capacity, uint32_t* count, char* buffer) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));
    const std::string* s = i ? pathString(i, path) : nullptr;

    if (!s)
      return XR_ERROR_PATH_INVALID;

    return copyString(capacity, count, buffer, *s);
  }

  // ---------------------------------------------------------------- system

  XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance, const XrSystemGetInfo* info, XrSystemId* system) {
    if (!info || !system)
      return XR_ERROR_VALIDATION_FAILURE;

    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
      return XR_ERROR_FORM_FACTOR_UNSUPPORTED;

    *system = SystemId;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId system, XrSystemProperties* properties) {
    if (system != SystemId || !properties)
      return XR_ERROR_SYSTEM_INVALID;

    properties->systemId = SystemId;
    properties->vendorId = 0;
    std::strcpy(properties->systemName, "FNVVR null headset");
    properties->graphicsProperties.maxSwapchainImageWidth  = 8192;
    properties->graphicsProperties.maxSwapchainImageHeight = 8192;
    properties->graphicsProperties.maxLayerCount           = settings().layers;
    properties->trackingProperties.orientationTracking     = XR_TRUE;
    properties->trackingProperties.positionTracking        = XR_TRUE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count,
      XrViewConfigurationType* types) {
    const XrViewConfigurationType stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return copyOut(capacity, count, types, &stereo, 1);
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType type,
      uint32_t capacity, uint32_t* count, XrViewConfigurationView* views) {
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
      return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    if (!count)
      return XR_ERROR_VALIDATION_FAILURE;

    *count = 2;

    if (!capacity)
      return XR_SUCCESS;

    if (capacity < 2)
      return XR_ERROR_SIZE_INSUFFICIENT;

    for (uint32_t i = 0; i < 2; i++) {
      views[i].recommendedImageRectWidth       = settings().width;
      views[i].recommendedImageRectHeight      = settings().height;
      views[i].maxImageRectWidth               = 8192;
      views[i].maxImageRectHeight              = 8192;
      views[i].recommendedSwapchainSampleCount = 1;
      views[i].maxSwapchainSampleCount         = 1;
    }
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType,
      uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes) {
    const XrEnvironmentBlendMode opaque = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return copyOut(capacity, count, modes, &opaque, 1);
  }

  // ---------------------------------------------------------------- Vulkan (XR_KHR_vulkan_enable)

  PFN_vkGetInstanceProcAddr vulkanLoader() {
    static HMODULE library = LoadLibraryW(L"vulkan-1.dll");
    return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr")) : nullptr;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsRequirementsKHR(XrInstance, XrSystemId system,
      XrGraphicsRequirementsVulkanKHR* requirements) {
    if (system != SystemId || !requirements)
      return XR_ERROR_VALIDATION_FAILURE;

    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanInstanceExtensionsKHR(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, char* buffer) {
    // Nothing is shared with another process: no extensions needed
    return copyString(capacity, count, buffer, std::string());
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanDeviceExtensionsKHR(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, char* buffer) {
    return copyString(capacity, count, buffer, std::string());
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetVulkanGraphicsDeviceKHR(XrInstance, XrSystemId system, VkInstance vkInstance,
      VkPhysicalDevice* physical) {
    if (system != SystemId || !vkInstance || !physical)
      return XR_ERROR_VALIDATION_FAILURE;

    PFN_vkGetInstanceProcAddr gipa = vulkanLoader();

    if (!gipa)
      return XR_ERROR_RUNTIME_FAILURE;

    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(vkInstance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(vkInstance, "vkGetPhysicalDeviceProperties"));

    if (!enumerate || !properties)
      return XR_ERROR_RUNTIME_FAILURE;

    uint32_t n = 0;

    if (enumerate(vkInstance, &n, nullptr) != VK_SUCCESS || !n)
      return XR_ERROR_RUNTIME_FAILURE;

    std::vector<VkPhysicalDevice> devices(n);
    enumerate(vkInstance, &n, devices.data());
    devices.resize(n);

    // The first discrete GPU, as a headset would be on it; else the first one
    VkPhysicalDevice chosen = devices[0];

    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties p;
      properties(d, &p);

      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        chosen = d;
        break;
      }
    }

    VkPhysicalDeviceProperties p;
    properties(chosen, &p);
    logLine("graphics device: %s", p.deviceName);
    *physical = chosen;
    return XR_SUCCESS;
  }

  bool loadVulkan(Vulkan& vk, const XrGraphicsBindingVulkanKHR& binding) {
    vk.getInstanceProcAddr = vulkanLoader();

    if (!vk.getInstanceProcAddr)
      return false;

    vk.instance = binding.instance;
    vk.physical = binding.physicalDevice;
    vk.device   = binding.device;

    auto gipa = vk.getInstanceProcAddr;
    vk.memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(vk.instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(vk.instance, "vkGetDeviceProcAddr"));

    if (!vk.memoryProperties || !gdpa)
      return false;

    vk.createImage       = reinterpret_cast<PFN_vkCreateImage>(gdpa(vk.device, "vkCreateImage"));
    vk.destroyImage      = reinterpret_cast<PFN_vkDestroyImage>(gdpa(vk.device, "vkDestroyImage"));
    vk.imageRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gdpa(vk.device, "vkGetImageMemoryRequirements"));
    vk.allocateMemory    = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(vk.device, "vkAllocateMemory"));
    vk.freeMemory        = reinterpret_cast<PFN_vkFreeMemory>(gdpa(vk.device, "vkFreeMemory"));
    vk.bindImageMemory   = reinterpret_cast<PFN_vkBindImageMemory>(gdpa(vk.device, "vkBindImageMemory"));

    if (!vk.createImage || !vk.destroyImage || !vk.imageRequirements || !vk.allocateMemory || !vk.freeMemory || !vk.bindImageMemory)
      return false;

    vk.memoryProperties(vk.physical, &vk.memory);
    return true;
  }

  // ---------------------------------------------------------------- session

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || i != g_instance || !info || !out)
      return XR_ERROR_HANDLE_INVALID;

    if (info->systemId != SystemId)
      return XR_ERROR_SYSTEM_INVALID;

    if (i->session)
      return XR_ERROR_LIMIT_REACHED;

    const XrGraphicsBindingVulkanKHR* binding = nullptr;

    for (auto* next = static_cast<const XrBaseInStructure*>(info->next); next; next = next->next) {
      if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
        binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
    }

    if (!binding || !binding->instance || !binding->physicalDevice || !binding->device)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;

    auto session = std::make_unique<Session>();
    session->instance = i;

    if (!loadVulkan(session->vk, *binding))
      return XR_ERROR_RUNTIME_FAILURE;

    double rate = settings().rate;
    session->period = rate > 0.0 ? int64_t(1e9 / rate) : int64_t(1e9 / 90.0);

    i->session = session.release();
    *out = XrSession(toHandle(i->session));

    setState(i->session, XR_SESSION_STATE_IDLE);
    setState(i->session, XR_SESSION_STATE_READY);
    logLine("session created (queue family %u)", binding->queueFamilyIndex);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s)
      return XR_ERROR_HANDLE_INVALID;

    logLine("session destroyed after %llu frames, %llu refused", (unsigned long long) s->framesTotal, (unsigned long long) s->refused);
    g_instance->session = nullptr;
    delete s;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s || !info)
      return XR_ERROR_HANDLE_INVALID;

    if (s->running)
      return XR_ERROR_SESSION_RUNNING;

    if (s->state != XR_SESSION_STATE_READY)
      return XR_ERROR_SESSION_NOT_READY;

    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
      return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    s->running  = true;
    s->nextWake = nowNs();
    s->windowStart = s->nextWake;
    setState(s, XR_SESSION_STATE_SYNCHRONIZED);
    setState(s, XR_SESSION_STATE_VISIBLE);
    setState(s, XR_SESSION_STATE_FOCUSED);
    logLine("session running");
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s)
      return XR_ERROR_HANDLE_INVALID;

    if (!s->running)
      return XR_ERROR_SESSION_NOT_RUNNING;

    s->running = false;
    s->waited  = false;
    s->begun   = false;
    setState(s, XR_SESSION_STATE_IDLE);
    logLine("session ended");
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s)
      return XR_ERROR_HANDLE_INVALID;

    if (!s->running)
      return XR_ERROR_SESSION_NOT_RUNNING;

    setState(s, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
  }

  // ---------------------------------------------------------------- spaces

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces) {
    const XrReferenceSpaceType types[] = { XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE };
    return copyOut(capacity, count, spaces, types, 3);
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* info, XrSpace* out) {
    if (!info || !out)
      return XR_ERROR_VALIDATION_FAILURE;

    if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW && info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL
     && info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE)
      return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;

    auto* space = new Space();
    space->session   = fromHandle<Session>(uint64_t(session));
    space->kind      = SpaceKind::Reference;
    space->reference = info->referenceSpaceType;
    space->offset    = fromXr(info->poseInReferenceSpace);

    // LOCAL starts at the head's place at eye height, like a seated origin; STAGE is on the floor
    if (space->reference == XR_REFERENCE_SPACE_TYPE_LOCAL)
      space->offset = compose({ {}, { 0.0f, HeadHeight, 0.0f } }, space->offset);

    *out = XrSpace(toHandle(space));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (!info || !out || !g_instance)
      return XR_ERROR_VALIDATION_FAILURE;

    auto* space = new Space();
    space->session = fromHandle<Session>(uint64_t(session));
    space->kind    = SpaceKind::Action;
    space->hand    = handOf(g_instance, info->subactionPath);
    space->offset  = fromXr(info->poseInActionSpace);
    *out = XrSpace(toHandle(space));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
    Space* s = fromHandle<Space>(uint64_t(space));

    if (!s)
      return XR_ERROR_HANDLE_INVALID;

    delete s;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime, XrSpaceLocation* location) {
    Space* s = fromHandle<Space>(uint64_t(space));
    Space* b = fromHandle<Space>(uint64_t(baseSpace));

    if (!s || !b || !location)
      return XR_ERROR_HANDLE_INVALID;

    Pose relative = compose(inverse(spacePose(b)), spacePose(s));
    location->pose = toXr(relative);
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT
      | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

    for (auto* next = static_cast<XrBaseOutStructure*>(location->next); next; next = next->next) {
      if (next->type == XR_TYPE_SPACE_VELOCITY) {
        auto* v = reinterpret_cast<XrSpaceVelocity*>(next);
        v->velocityFlags   = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        v->linearVelocity  = { 0, 0, 0 };
        v->angularVelocity = { 0, 0, 0 };
      }
    }
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession, const XrViewLocateInfo* info, XrViewState* state,
      uint32_t capacity, uint32_t* count, XrView* views) {
    if (!info || !state || !count)
      return XR_ERROR_VALIDATION_FAILURE;

    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
      return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;

    Space* base = fromHandle<Space>(uint64_t(info->space));

    if (!base)
      return XR_ERROR_HANDLE_INVALID;

    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT
      | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;

    if (!capacity)
      return XR_SUCCESS;

    if (capacity < 2)
      return XR_ERROR_SIZE_INSUFFICIENT;

    Pose toBase = inverse(spacePose(base));

    for (uint32_t i = 0; i < 2; i++) {
      Pose eye = compose(HeadPose, { {}, { i ? EyeOffset : -EyeOffset, 0.0f, 0.0f } });
      views[i].pose = toXr(compose(toBase, eye));
      views[i].fov  = EyeFov;
    }
    return XR_SUCCESS;
  }

  // ---------------------------------------------------------------- actions

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* info, XrActionSet* out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || !info || !out)
      return XR_ERROR_VALIDATION_FAILURE;

    i->actionSets.push_back(std::make_unique<ActionSet>());
    i->actionSets.back()->name = info->actionSetName;
    *out = XrActionSet(toHandle(i->actionSets.back().get()));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet set) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ActionSet* s = fromHandle<ActionSet>(uint64_t(set));

    if (!s || !g_instance)
      return XR_ERROR_HANDLE_INVALID;

    auto& actions = g_instance->actions;
    actions.erase(std::remove_if(actions.begin(), actions.end(), [s] (const auto& a) { return a->set == s; }), actions.end());

    auto& sets = g_instance->actionSets;
    sets.erase(std::remove_if(sets.begin(), sets.end(), [s] (const auto& a) { return a.get() == s; }), sets.end());
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (!g_instance || !info || !out)
      return XR_ERROR_VALIDATION_FAILURE;

    g_instance->actions.push_back(std::make_unique<Action>());
    Action* a = g_instance->actions.back().get();
    a->set  = fromHandle<ActionSet>(uint64_t(set));
    a->type = info->actionType;
    a->name = info->actionName;
    *out = XrAction(toHandle(a));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Action* a = fromHandle<Action>(uint64_t(action));

    if (!a || !g_instance)
      return XR_ERROR_HANDLE_INVALID;

    auto& actions = g_instance->actions;
    actions.erase(std::remove_if(actions.begin(), actions.end(), [a] (const auto& x) { return x.get() == a; }), actions.end());
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* info) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Instance* i = fromHandle<Instance>(uint64_t(instance));

    if (!i || !info || !pathString(i, info->interactionProfile))
      return XR_ERROR_PATH_INVALID;

    // The first profile suggested is the one the "controllers" are
    if (!i->profile)
      i->profile = info->interactionProfile;

    logLine("bindings suggested for %s, %u inputs", pathString(i, info->interactionProfile)->c_str(), info->countSuggestedBindings);
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo*) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance)
      return XR_ERROR_HANDLE_INVALID;

    XrEventDataInteractionProfileChanged e = { XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED };
    e.session = session;
    pushEvent(g_instance, &e, sizeof(e));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession, XrPath topLevelUserPath, XrInteractionProfileState* state) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    if (!g_instance || !state)
      return XR_ERROR_VALIDATION_FAILURE;

    state->interactionProfile = handOf(g_instance, topLevelUserPath) >= 0 ? g_instance->profile : XR_NULL_PATH;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*) {
    return XR_SUCCESS;
  }

  // The controllers are idle: active, nothing pressed; the test channel presses what a test needs
  XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* state) {
    if (!state)
      return XR_ERROR_VALIDATION_FAILURE;

    state->currentState = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo*, XrActionStateFloat* state) {
    if (!state)
      return XR_ERROR_VALIDATION_FAILURE;

    state->currentState = 0.0f;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f* state) {
    if (!state)
      return XR_ERROR_VALIDATION_FAILURE;

    state->currentState = { 0.0f, 0.0f };
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    state->isActive = XR_TRUE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession, const XrActionStateGetInfo*, XrActionStatePose* state) {
    if (!state)
      return XR_ERROR_VALIDATION_FAILURE;

    state->isActive = XR_TRUE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession, const XrHapticActionInfo*, const XrHapticBaseHeader*) {
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession, const XrHapticActionInfo*) {
    return XR_SUCCESS;
  }

  // ---------------------------------------------------------------- swapchains

  const int64_t Formats[] = {
    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats) {
    return copyOut(capacity, count, formats, Formats, uint32_t(std::size(Formats)));
  }

  void destroyImages(Swapchain* chain) {
    const Vulkan& vk = chain->session->vk;

    for (SwapchainImage& i : chain->images) {
      if (i.image)
        vk.destroyImage(vk.device, i.image, nullptr);
      if (i.memory)
        vk.freeMemory(vk.device, i.memory, nullptr);
    }
    chain->images.clear();
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s || !info || !out)
      return XR_ERROR_HANDLE_INVALID;

    if (std::find(std::begin(Formats), std::end(Formats), info->format) == std::end(Formats))
      return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;

    if (!info->width || !info->height || info->width > 8192 || info->height > 8192 || info->faceCount != 1 || info->arraySize < 1)
      return XR_ERROR_VALIDATION_FAILURE;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)  usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)      usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT)      usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)           usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)  usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    auto chain = std::make_unique<Swapchain>();
    chain->session = s;
    chain->width   = info->width;
    chain->height  = info->height;
    chain->format  = info->format;

    const Vulkan& vk = s->vk;

    for (uint32_t n = 0; n < 3; n++) {
      VkImageCreateInfo image = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
      image.flags         = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      image.imageType     = VK_IMAGE_TYPE_2D;
      image.format        = VkFormat(info->format);
      image.extent        = { info->width, info->height, 1 };
      image.mipLevels     = std::max(info->mipCount, 1u);
      image.arrayLayers   = info->arraySize;
      image.samples       = VK_SAMPLE_COUNT_1_BIT;
      image.tiling        = VK_IMAGE_TILING_OPTIMAL;
      image.usage         = usage;
      image.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
      image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      SwapchainImage entry;

      if (vk.createImage(vk.device, &image, nullptr, &entry.image) != VK_SUCCESS) {
        destroyImages(chain.get());
        return XR_ERROR_RUNTIME_FAILURE;
      }

      chain->images.push_back(entry);

      VkMemoryRequirements need;
      vk.imageRequirements(vk.device, entry.image, &need);

      uint32_t type = UINT32_MAX;

      for (uint32_t t = 0; t < vk.memory.memoryTypeCount && type == UINT32_MAX; t++) {
        if ((need.memoryTypeBits & (1u << t)) && (vk.memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
          type = t;
      }

      for (uint32_t t = 0; t < vk.memory.memoryTypeCount && type == UINT32_MAX; t++) {
        if (need.memoryTypeBits & (1u << t))
          type = t;
      }

      VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      allocate.allocationSize  = need.size;
      allocate.memoryTypeIndex = type;

      if (type == UINT32_MAX
       || vk.allocateMemory(vk.device, &allocate, nullptr, &chain->images.back().memory) != VK_SUCCESS
       || vk.bindImageMemory(vk.device, entry.image, chain->images.back().memory, 0) != VK_SUCCESS) {
        destroyImages(chain.get());
        return XR_ERROR_RUNTIME_FAILURE;
      }
    }

    logLine("swapchain %ux%u format %lld", info->width, info->height, (long long) info->format);
    *out = XrSwapchain(toHandle(chain.release()));
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Swapchain* chain = fromHandle<Swapchain>(uint64_t(swapchain));

    if (!chain)
      return XR_ERROR_HANDLE_INVALID;

    destroyImages(chain);
    delete chain;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count,
      XrSwapchainImageBaseHeader* images) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Swapchain* chain = fromHandle<Swapchain>(uint64_t(swapchain));

    if (!chain || !count)
      return XR_ERROR_HANDLE_INVALID;

    *count = uint32_t(chain->images.size());

    if (!capacity)
      return XR_SUCCESS;

    if (capacity < *count)
      return XR_ERROR_SIZE_INSUFFICIENT;

    auto* out = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);

    for (uint32_t i = 0; i < *count; i++) {
      if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
      out[i].image = chain->images[i].image;
    }
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Swapchain* chain = fromHandle<Swapchain>(uint64_t(swapchain));

    if (!chain || !index)
      return XR_ERROR_HANDLE_INVALID;

    if (chain->acquired.size() >= chain->images.size())
      return XR_ERROR_CALL_ORDER_INVALID;

    *index = chain->next;
    chain->acquired.push_back(chain->next);
    chain->next = (chain->next + 1) % uint32_t(chain->images.size());
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Swapchain* chain = fromHandle<Swapchain>(uint64_t(swapchain));

    if (!chain)
      return XR_ERROR_HANDLE_INVALID;

    if (chain->acquired.empty() || chain->waited)
      return XR_ERROR_CALL_ORDER_INVALID;

    chain->waited = true;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Swapchain* chain = fromHandle<Swapchain>(uint64_t(swapchain));

    if (!chain)
      return XR_ERROR_HANDLE_INVALID;

    if (!chain->waited)
      return XR_ERROR_CALL_ORDER_INVALID;

    chain->acquired.pop_front();
    chain->waited = false;
    return XR_SUCCESS;
  }

  // ---------------------------------------------------------------- frames

  void logWindow(Session* s, int64_t now) {
    double seconds = double(now - s->windowStart) * 1e-9;

    if (seconds < 10.0)
      return;

    logLine("frames: %.1f per second, layers %.1f average / %u most, %llu refused, %llu without the eyes (total %llu)",
      double(s->frames) / seconds, s->frames ? double(s->layerSum) / double(s->frames) : 0.0, s->layerMax,
      (unsigned long long) s->refused, (unsigned long long) s->noProjection, (unsigned long long) s->framesTotal);

    s->windowStart = now;
    s->frames = 0;
    s->layerSum = 0;
    s->layerMax = 0;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo*, XrFrameState* state) {
    int64_t wake = 0;
    int64_t period = 0;

    {
      std::lock_guard<std::recursive_mutex> lock(g_mutex);
      Session* s = fromHandle<Session>(uint64_t(session));

      if (!s || !g_instance || g_instance->session != s || !state)
        return XR_ERROR_HANDLE_INVALID;

      if (!s->running)
        return XR_ERROR_SESSION_NOT_RUNNING;

      period = s->period;
      int64_t now = nowNs();

      if (settings().rate > 0.0) {
        // Behind by more than a frame: start again from now, as a compositor would skip
        if (s->nextWake < now - period)
          s->nextWake = now;

        wake = s->nextWake;
        s->nextWake += period;
      }
    }

    if (wake)
      sleepUntilNs(wake);

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s)
      return XR_ERROR_HANDLE_INVALID;

    s->display = nowNs() + period;
    s->waited  = true;

    state->predictedDisplayTime   = s->display;
    state->predictedDisplayPeriod = period;
    state->shouldRender           = s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo*) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s)
      return XR_ERROR_HANDLE_INVALID;

    if (!s->running)
      return XR_ERROR_SESSION_NOT_RUNNING;

    if (!s->waited)
      return XR_ERROR_CALL_ORDER_INVALID;

    bool discarded = s->begun;
    s->waited = false;
    s->begun  = true;
    return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
  }

  XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Session* s = fromHandle<Session>(uint64_t(session));

    if (!s || !g_instance || g_instance->session != s || !info)
      return XR_ERROR_HANDLE_INVALID;

    if (!s->running)
      return XR_ERROR_SESSION_NOT_RUNNING;

    if (!s->begun)
      return XR_ERROR_CALL_ORDER_INVALID;

    s->begun = false;

    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
      return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;

    if (info->layerCount > settings().layers) {
      if (!s->refused)
        logLine("frame refused: %u layers, the limit is %u", info->layerCount, settings().layers);
      s->refused += 1;
      return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    }

    bool projection = false;

    for (uint32_t i = 0; i < info->layerCount; i++) {
      const XrCompositionLayerBaseHeader* layer = info->layers[i];

      if (!layer)
        return XR_ERROR_LAYER_INVALID;

      if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(layer);

        if (p->viewCount != 2)
          return XR_ERROR_VALIDATION_FAILURE;
        projection = true;
      } else if (layer->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
        return XR_ERROR_LAYER_INVALID;
      }
    }

    if (!s->framesTotal)
      logLine("first frame: %u layers%s", info->layerCount, projection ? "" : " (no eyes)");

    s->frames      += 1;
    s->framesTotal += 1;
    s->layerSum    += info->layerCount;
    s->layerMax     = std::max(s->layerMax, info->layerCount);
    s->noProjection += projection ? 0 : 1;
    logWindow(s, nowNs());
    return XR_SUCCESS;
  }

  // ---------------------------------------------------------------- entry points

  struct Entry { const char* name; PFN_xrVoidFunction function; };

#define ENTRY(f) { #f, reinterpret_cast<PFN_xrVoidFunction>(f) }

  const Entry Entries[] = {
    ENTRY(xrGetInstanceProcAddr),
    ENTRY(xrEnumerateInstanceExtensionProperties),
    ENTRY(xrCreateInstance),
    ENTRY(xrDestroyInstance),
    ENTRY(xrGetInstanceProperties),
    ENTRY(xrResultToString),
    ENTRY(xrPollEvent),
    ENTRY(xrStringToPath),
    ENTRY(xrPathToString),
    ENTRY(xrGetSystem),
    ENTRY(xrGetSystemProperties),
    ENTRY(xrEnumerateViewConfigurations),
    ENTRY(xrEnumerateViewConfigurationViews),
    ENTRY(xrEnumerateEnvironmentBlendModes),
    ENTRY(xrGetVulkanGraphicsRequirementsKHR),
    ENTRY(xrGetVulkanInstanceExtensionsKHR),
    ENTRY(xrGetVulkanDeviceExtensionsKHR),
    ENTRY(xrGetVulkanGraphicsDeviceKHR),
    ENTRY(xrCreateSession),
    ENTRY(xrDestroySession),
    ENTRY(xrBeginSession),
    ENTRY(xrEndSession),
    ENTRY(xrRequestExitSession),
    ENTRY(xrEnumerateReferenceSpaces),
    ENTRY(xrCreateReferenceSpace),
    ENTRY(xrCreateActionSpace),
    ENTRY(xrDestroySpace),
    ENTRY(xrLocateSpace),
    ENTRY(xrLocateViews),
    ENTRY(xrCreateActionSet),
    ENTRY(xrDestroyActionSet),
    ENTRY(xrCreateAction),
    ENTRY(xrDestroyAction),
    ENTRY(xrSuggestInteractionProfileBindings),
    ENTRY(xrAttachSessionActionSets),
    ENTRY(xrGetCurrentInteractionProfile),
    ENTRY(xrSyncActions),
    ENTRY(xrGetActionStateBoolean),
    ENTRY(xrGetActionStateFloat),
    ENTRY(xrGetActionStateVector2f),
    ENTRY(xrGetActionStatePose),
    ENTRY(xrApplyHapticFeedback),
    ENTRY(xrStopHapticFeedback),
    ENTRY(xrEnumerateSwapchainFormats),
    ENTRY(xrCreateSwapchain),
    ENTRY(xrDestroySwapchain),
    ENTRY(xrEnumerateSwapchainImages),
    ENTRY(xrAcquireSwapchainImage),
    ENTRY(xrWaitSwapchainImage),
    ENTRY(xrReleaseSwapchainImage),
    ENTRY(xrWaitFrame),
    ENTRY(xrBeginFrame),
    ENTRY(xrEndFrame),
  };

#undef ENTRY

  XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function)
      return XR_ERROR_VALIDATION_FAILURE;

    for (const Entry& e : Entries) {
      if (!std::strcmp(e.name, name)) {
        *function = e.function;
        return XR_SUCCESS;
      }
    }

    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }

}

// Loaded at all? One line when a process loads the DLL (with FNVVR_NULL_XR_LOG set)
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    char exe[MAX_PATH] = { };
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    logLine("loaded into %s", exe);
  }
  return TRUE;
}

// The loader's entry point (exported undecorated through fnvvr_null_xr.def)
extern "C" XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* request) {
  if (!loaderInfo || !request
   || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO
   || loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION
   || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)
   || request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST
   || request->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION
   || request->structSize != sizeof(XrNegotiateRuntimeRequest)
   || loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION
   || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
    return XR_ERROR_INITIALIZATION_FAILED;

  request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  request->runtimeApiVersion       = std::min<XrVersion>(XR_CURRENT_API_VERSION, loaderInfo->maxApiVersion);
  request->getInstanceProcAddr     = xrGetInstanceProcAddr;
  logLine("loaded by the OpenXR loader");
  return XR_SUCCESS;
}
