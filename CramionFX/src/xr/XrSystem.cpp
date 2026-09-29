#include "CramionFX/xr/XrSystem.h"

#include <CramionDM/Input.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

#if defined(CRAMION_XR)
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

namespace cramion::xr {

using core::Quat;
using core::Vec3;

// -----------------------------------------------------------------------------
// Utilidades
// -----------------------------------------------------------------------------

Quat multiply(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = core::cross(u, v) * 2.0f;
    return v + t * q.w + core::cross(u, t);
}

core::Mat4 poseMatrix(const Pose& pose) { return core::composeTrs(pose.position, pose.orientation, Vec3{1.0f, 1.0f, 1.0f}); }

const char* buttonName(Button button) {
    switch (button) {
        case Button::Trigger: return "trigger";
        case Button::Grip: return "grip";
        case Button::Thumbstick: return "thumbstick";
        case Button::Primary: return "primary";
        case Button::Secondary: return "secondary";
        case Button::Menu: return "menu";
        default: return "?";
    }
}

void XrSystem::applyToInput(dm::Input& input) const {
    // Sin sesion o sin foco: todo suelto (las transiciones salen igual).
    const bool on = focused();
    for (int h = 0; h < 2; ++h) {
        const Controller& c = controller(static_cast<Hand>(h));
        const bool live = on && c.active;
        const int b0 = h * 6;
        const int a0 = h * 4;
        for (int b = 0; b < 6; ++b) {
            input.setXrButton(static_cast<dm::XrButton>(b0 + b), live && c.buttons[static_cast<std::size_t>(b)]);
        }
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 0), live ? c.trigger : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 1), live ? c.grip_value : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 2), live ? c.thumbstick.x : 0.0f);
        input.setXrAxis(static_cast<dm::XrAxis>(a0 + 3), live ? c.thumbstick.y : 0.0f);
    }
}

#if defined(CRAMION_XR)

// -----------------------------------------------------------------------------
// OpenXR
// -----------------------------------------------------------------------------

namespace {

std::vector<std::string> splitSpaces(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

Pose toPose(const XrPosef& p, bool valid) {
    Pose out;
    out.position = Vec3{p.position.x, p.position.y, p.position.z};
    out.orientation = Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.valid = valid;
    return out;
}

}  // namespace

struct XrSystem::Impl {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace stage_space = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool session_running = false;
    bool exit_requested = false;
    TrackingOrigin origin = TrackingOrigin::Floor;
    std::string error;
    std::string runtime_name;
    std::string system_name;

    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::uint32_t acquired = 0;
        bool holding = false;
        bool drawn = false;
    };
    std::array<EyeSwapchain, 2> eyes_sc;
    VkExtent2D extent{0, 0};
    VkFormat format = VK_FORMAT_UNDEFINED;

    // Frame.
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    bool frame_begun = false;
    std::array<XrView, 2> views{};
    std::array<EyeView, 2> eye_views{};
    Pose head;

    // Mandos.
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction grip_pose = XR_NULL_HANDLE, aim_pose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE,
             thumbstick = XR_NULL_HANDLE, thumbstick_click = XR_NULL_HANDLE, primary = XR_NULL_HANDLE,
             secondary = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    std::array<XrPath, 2> hand_paths{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> grip_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2> aim_spaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<Controller, 2> controllers{};

    bool ok(XrResult r, const char* what) {
        if (XR_SUCCEEDED(r)) return true;
        char text[XR_MAX_RESULT_STRING_SIZE] = {};
        if (instance != XR_NULL_HANDLE) xrResultToString(instance, r, text);
        error = std::string(what) + ": " + (text[0] != '\0' ? text : std::to_string(static_cast<int>(r)));
        std::cerr << "[VR] " << error << "\n";
        return false;
    }

    XrPath path(const char* text) const {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance, text, &p);
        return p;
    }

    XrSpace appSpace() const {
        return origin == TrackingOrigin::Floor && stage_space != XR_NULL_HANDLE ? stage_space : local_space;
    }

    XrAction makeAction(const char* name, const char* label, XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = hand_paths.data();
        XrAction action = XR_NULL_HANDLE;
        ok(xrCreateAction(action_set, &info, &action), name);
        return action;
    }

    // Cada perfil por separado: si un runtime no conoce uno, los demas valen.
    void suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings) {
        std::vector<XrActionSuggestedBinding> list;
        for (const auto& [action, p] : bindings) {
            if (action != XR_NULL_HANDLE) list.push_back({action, path(p)});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = path(profile);
        info.countSuggestedBindings = static_cast<std::uint32_t>(list.size());
        info.suggestedBindings = list.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &info);
        if (XR_FAILED(r)) std::cerr << "[VR] Perfil de mandos no aceptado: " << profile << " (" << static_cast<int>(r) << ")\n";
    }

    void createActions() {
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(set_info.actionSetName, "cramion", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(set_info.localizedActionSetName, "Cramion", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        if (!ok(xrCreateActionSet(instance, &set_info, &action_set), "xrCreateActionSet")) return;
        hand_paths = {path("/user/hand/left"), path("/user/hand/right")};
        grip_pose = makeAction("grip_pose", "Mano", XR_ACTION_TYPE_POSE_INPUT);
        aim_pose = makeAction("aim_pose", "Apuntar", XR_ACTION_TYPE_POSE_INPUT);
        trigger = makeAction("trigger", "Gatillo", XR_ACTION_TYPE_FLOAT_INPUT);
        squeeze = makeAction("squeeze", "Agarre", XR_ACTION_TYPE_FLOAT_INPUT);
        thumbstick = makeAction("thumbstick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
        thumbstick_click = makeAction("thumbstick_click", "Pulsar el stick", XR_ACTION_TYPE_BOOLEAN_INPUT);
        primary = makeAction("primary", "Boton A/X", XR_ACTION_TYPE_BOOLEAN_INPUT);
        secondary = makeAction("secondary", "Boton B/Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
        menu = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
        haptic = makeAction("haptic", "Vibracion", XR_ACTION_TYPE_VIBRATION_OUTPUT);

        const auto both = [](std::vector<std::pair<XrAction, const char*>>& list, XrAction a, const char* left,
                             const char* right) {
            list.push_back({a, left});
            list.push_back({a, right});
        };
        {  // Cualquier mando (el perfil minimo)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/select/click", "/user/hand/right/input/select/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/khr/simple_controller", b);
        }
        {  // Meta / Oculus Touch (Quest por Link o Air Link)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/x/click", "/user/hand/right/input/a/click");
            both(b, secondary, "/user/hand/left/input/y/click", "/user/hand/right/input/b/click");
            b.push_back({menu, "/user/hand/left/input/menu/click"});
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/oculus/touch_controller", b);
        }
        {  // Valve Index
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/a/click", "/user/hand/right/input/a/click");
            both(b, secondary, "/user/hand/left/input/b/click", "/user/hand/right/input/b/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/valve/index_controller", b);
        }
        {  // HTC Vive (el trackpad hace de stick)
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click");
            both(b, thumbstick, "/user/hand/left/input/trackpad", "/user/hand/right/input/trackpad");
            both(b, thumbstick_click, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/htc/vive_controller", b);
        }
        {  // Windows Mixed Reality
            std::vector<std::pair<XrAction, const char*>> b;
            both(b, grip_pose, "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose");
            both(b, aim_pose, "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose");
            both(b, trigger, "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value");
            both(b, squeeze, "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click");
            both(b, thumbstick, "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick");
            both(b, thumbstick_click, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click");
            both(b, primary, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click");
            both(b, menu, "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click");
            both(b, haptic, "/user/hand/left/output/haptic", "/user/hand/right/output/haptic");
            suggest("/interaction_profiles/microsoft/motion_controller", b);
        }
    }

    void createActionSpaces() {
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.subactionPath = hand_paths[static_cast<std::size_t>(h)];
            info.poseInActionSpace.orientation.w = 1.0f;
            info.action = grip_pose;
            if (grip_pose != XR_NULL_HANDLE) xrCreateActionSpace(session, &info, &grip_spaces[static_cast<std::size_t>(h)]);
            info.action = aim_pose;
            if (aim_pose != XR_NULL_HANDLE) xrCreateActionSpace(session, &info, &aim_spaces[static_cast<std::size_t>(h)]);
        }
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &action_set;
        ok(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
    }

    Pose locate(XrSpace space, XrTime time) const {
        if (space == XR_NULL_HANDLE) return {};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(space, appSpace(), time, &location))) return {};
        const bool valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                           (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        return toPose(location.pose, valid);
    }

    float getFloat(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return 0.0f;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session, &info, &state)) || !state.isActive) return 0.0f;
        return state.currentState;
    }

    bool getBool(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session, &info, &state)) || !state.isActive) return false;
        return state.currentState == XR_TRUE;
    }

    core::Vec2 getVec2(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return {};
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(session, &info, &state)) || !state.isActive) return {};
        return core::Vec2{state.currentState.x, state.currentState.y};
    }

    bool poseActive(XrAction action, int hand) const {
        if (action == XR_NULL_HANDLE) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = hand_paths[static_cast<std::size_t>(hand)];
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        return XR_SUCCEEDED(xrGetActionStatePose(session, &info, &state)) && state.isActive;
    }

    void updateControllers(XrTime time) {
        if (action_set == XR_NULL_HANDLE) return;
        const bool can_read = state == XR_SESSION_STATE_FOCUSED;
        if (can_read) {
            XrActiveActionSet active{action_set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            xrSyncActions(session, &sync);
        }
        for (int h = 0; h < 2; ++h) {
            Controller& c = controllers[static_cast<std::size_t>(h)];
            const Controller previous = c;
            c = Controller{};
            if (!can_read) continue;
            c.active = poseActive(grip_pose, h);
            c.grip = locate(grip_spaces[static_cast<std::size_t>(h)], time);
            c.aim = locate(aim_spaces[static_cast<std::size_t>(h)], time);
            c.trigger = std::clamp(getFloat(trigger, h), 0.0f, 1.0f);
            c.grip_value = std::clamp(getFloat(squeeze, h), 0.0f, 1.0f);
            c.thumbstick = getVec2(thumbstick, h);
            // Gatillo y agarre con histeresis (no parpadean en la mitad).
            const auto analog = [](float v, bool was) { return was ? v > 0.4f : v > 0.6f; };
            c.buttons[static_cast<std::size_t>(Button::Trigger)] =
                analog(c.trigger, previous.buttons[static_cast<std::size_t>(Button::Trigger)]);
            c.buttons[static_cast<std::size_t>(Button::Grip)] =
                analog(c.grip_value, previous.buttons[static_cast<std::size_t>(Button::Grip)]);
            c.buttons[static_cast<std::size_t>(Button::Thumbstick)] = getBool(thumbstick_click, h);
            c.buttons[static_cast<std::size_t>(Button::Primary)] = getBool(primary, h);
            c.buttons[static_cast<std::size_t>(Button::Secondary)] = getBool(secondary, h);
            c.buttons[static_cast<std::size_t>(Button::Menu)] = getBool(menu, h);
        }
    }

    void pollEvents() {
        if (instance == XR_NULL_HANDLE) return;
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            switch (event.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                    state = changed.state;
                    if (state == XR_SESSION_STATE_READY && session != XR_NULL_HANDLE) {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (ok(xrBeginSession(session, &begin), "xrBeginSession")) {
                            session_running = true;
                            std::cout << "[VR] Sesion en marcha\n";
                        }
                    } else if (state == XR_SESSION_STATE_STOPPING) {
                        xrEndSession(session);
                        session_running = false;
                        frame_begun = false;
                        std::cout << "[VR] Sesion parada\n";
                    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                        session_running = false;
                        exit_requested = state == XR_SESSION_STATE_EXITING;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    session_running = false;
                    exit_requested = true;
                    break;
                default: break;
            }
            event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void endFrameNow() {
        if (!frame_begun) return;
        frame_begun = false;
        for (EyeSwapchain& e : eyes_sc) {
            if (e.holding) {
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(e.handle, &release);
                e.holding = false;
            }
        }
        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
        const bool drawn = frame_state.shouldRender && eyes_sc[0].drawn && eyes_sc[1].drawn;
        if (drawn) {
            for (int i = 0; i < 2; ++i) {
                XrCompositionLayerProjectionView& v = projection_views[static_cast<std::size_t>(i)];
                v = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                v.pose = views[static_cast<std::size_t>(i)].pose;
                v.fov = views[static_cast<std::size_t>(i)].fov;
                v.subImage.swapchain = eyes_sc[static_cast<std::size_t>(i)].handle;
                v.subImage.imageRect.offset = {0, 0};
                v.subImage.imageRect.extent = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)};
                v.subImage.imageArrayIndex = 0;
            }
            layer.space = appSpace();
            layer.viewCount = 2;
            layer.views = projection_views.data();
        }
        eyes_sc[0].drawn = eyes_sc[1].drawn = false;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = drawn ? 1 : 0;
        end.layers = drawn ? layers : nullptr;
        ok(xrEndFrame(session, &end), "xrEndFrame");
    }

    void destroy() {
        if (session != XR_NULL_HANDLE && frame_begun) endFrameNow();
        for (EyeSwapchain& e : eyes_sc) {
            if (e.handle != XR_NULL_HANDLE) xrDestroySwapchain(e.handle);
            e = EyeSwapchain{};
        }
        for (XrSpace& s : grip_spaces) {
            if (s != XR_NULL_HANDLE) xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
        for (XrSpace& s : aim_spaces) {
            if (s != XR_NULL_HANDLE) xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
        for (XrSpace* s : {&stage_space, &local_space, &view_space}) {
            if (*s != XR_NULL_HANDLE) xrDestroySpace(*s);
            *s = XR_NULL_HANDLE;
        }
        if (action_set != XR_NULL_HANDLE) xrDestroyActionSet(action_set);  // destruye sus acciones
        action_set = XR_NULL_HANDLE;
        if (session != XR_NULL_HANDLE) {
            if (session_running) xrEndSession(session);
            xrDestroySession(session);
        }
        session = XR_NULL_HANDLE;
        session_running = false;
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        system = XR_NULL_SYSTEM_ID;
    }
};

XrSystem::XrSystem() : impl_(std::make_unique<Impl>()) {}
XrSystem::~XrSystem() { shutdown(); }

bool XrSystem::compiled() { return true; }

bool XrSystem::createInstance(const char* app_name) {
    Impl& d = *impl_;
    if (d.instance != XR_NULL_HANDLE) return true;

    std::uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
        d.error = "No hay un runtime de OpenXR instalado (SteamVR, Meta Quest Link, Windows Mixed Reality...)";
        std::cerr << "[VR] " << d.error << "\n";
        return false;
    }
    std::vector<XrExtensionProperties> available(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
    const bool vulkan = std::any_of(available.begin(), available.end(), [](const XrExtensionProperties& p) {
        return std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!vulkan) {
        d.error = "El runtime de OpenXR no admite Vulkan (XR_KHR_vulkan_enable)";
        std::cerr << "[VR] " << d.error << "\n";
        return false;
    }

    const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, app_name != nullptr ? app_name : "Cramion",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    std::strncpy(info.applicationInfo.engineName, "Cramion Engine", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    if (!d.ok(xrCreateInstance(&info, &d.instance), "xrCreateInstance")) {
        d.instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(d.instance, &properties))) {
        d.runtime_name = properties.runtimeName;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(d.instance, &system_info, &d.system))) {
        d.error = "No hay un casco de VR conectado (" + d.runtime_name + ")";
        std::cerr << "[VR] " << d.error << "\n";
        d.destroy();
        return false;
    }
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(d.instance, d.system, &system_properties))) {
        d.system_name = system_properties.systemName;
    }

    xrGetInstanceProcAddr(d.instance, "xrGetVulkanInstanceExtensionsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&d.get_instance_extensions));
    xrGetInstanceProcAddr(d.instance, "xrGetVulkanDeviceExtensionsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&d.get_device_extensions));
    xrGetInstanceProcAddr(d.instance, "xrGetVulkanGraphicsDeviceKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&d.get_graphics_device));
    xrGetInstanceProcAddr(d.instance, "xrGetVulkanGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&d.get_graphics_requirements));
    if (d.get_instance_extensions == nullptr || d.get_device_extensions == nullptr || d.get_graphics_device == nullptr ||
        d.get_graphics_requirements == nullptr) {
        d.error = "El runtime no da las funciones de Vulkan";
        d.destroy();
        return false;
    }
    // Obligatorio antes de crear la sesion.
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    d.get_graphics_requirements(d.instance, d.system, &requirements);

    // Las acciones se crean con la instancia (antes de la sesion).
    d.createActions();
    std::cout << "[VR] OpenXR: " << d.runtime_name << ", casco: " << d.system_name << "\n";
    return true;
}

std::vector<std::string> XrSystem::requiredInstanceExtensions() const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return {};
    std::uint32_t size = 0;
    d.get_instance_extensions(d.instance, d.system, 0, &size, nullptr);
    std::string text(size, '\0');
    d.get_instance_extensions(d.instance, d.system, size, &size, text.data());
    return splitSpaces(text.c_str());
}

std::vector<std::string> XrSystem::requiredDeviceExtensions() const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return {};
    std::uint32_t size = 0;
    d.get_device_extensions(d.instance, d.system, 0, &size, nullptr);
    std::string text(size, '\0');
    d.get_device_extensions(d.instance, d.system, size, &size, text.data());
    return splitSpaces(text.c_str());
}

VkPhysicalDevice XrSystem::physicalDevice(VkInstance instance) const {
    const Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return VK_NULL_HANDLE;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (XR_FAILED(d.get_graphics_device(d.instance, d.system, instance, &device))) return VK_NULL_HANDLE;
    return device;
}

bool XrSystem::createSession(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                             std::uint32_t queue_family, std::uint32_t queue_index) {
    Impl& d = *impl_;
    if (d.instance == XR_NULL_HANDLE) return false;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family;
    binding.queueIndex = queue_index;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = d.system;
    if (!d.ok(xrCreateSession(d.instance, &info, &d.session), "xrCreateSession")) {
        d.session = XR_NULL_HANDLE;
        return false;
    }

    // Espacios: suelo (si el casco lo tiene), sentado y la cabeza.
    std::uint32_t space_count = 0;
    xrEnumerateReferenceSpaces(d.session, 0, &space_count, nullptr);
    std::vector<XrReferenceSpaceType> spaces(space_count);
    xrEnumerateReferenceSpaces(d.session, space_count, &space_count, spaces.data());
    const auto make_space = [&](XrReferenceSpaceType type, XrSpace& out) {
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = type;
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        xrCreateReferenceSpace(d.session, &space_info, &out);
    };
    if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end()) {
        make_space(XR_REFERENCE_SPACE_TYPE_STAGE, d.stage_space);
    }
    make_space(XR_REFERENCE_SPACE_TYPE_LOCAL, d.local_space);
    make_space(XR_REFERENCE_SPACE_TYPE_VIEW, d.view_space);

    // Tamano de cada ojo (el recomendado por el runtime).
    std::uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config(view_count, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(d.instance, d.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                                      &view_count, config.data());
    if (view_count < 2) {
        d.error = "El casco no tiene vista estereo";
        return false;
    }
    d.extent = VkExtent2D{config[0].recommendedImageRectWidth, config[0].recommendedImageRectHeight};

    // Formato: sRGB de 8 bits (el motor ya escribe con gamma; se copia tal cual).
    std::uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(d.session, 0, &format_count, nullptr);
    std::vector<std::int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(d.session, format_count, &format_count, formats.data());
    for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(wanted)) != formats.end()) {
            d.format = wanted;
            break;
        }
    }
    if (d.format == VK_FORMAT_UNDEFINED) {
        d.error = "El runtime no ofrece swapchains sRGB de 8 bits";
        std::cerr << "[VR] " << d.error << "\n";
        return false;
    }
    for (Impl::EyeSwapchain& eye : d.eyes_sc) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = d.format;
        sc.sampleCount = 1;
        sc.width = d.extent.width;
        sc.height = d.extent.height;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        if (!d.ok(xrCreateSwapchain(d.session, &sc, &eye.handle), "xrCreateSwapchain")) return false;
        std::uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eye.handle, 0, &image_count, nullptr);
        eye.images.assign(image_count, XrSwapchainImageVulkanKHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(eye.handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    }
    d.createActionSpaces();
    for (XrView& v : d.views) v = XrView{XR_TYPE_VIEW};
    std::cout << "[VR] Sesion creada: " << d.extent.width << " x " << d.extent.height << " por ojo\n";
    return true;
}

void XrSystem::shutdown() { impl_->destroy(); }

const std::string& XrSystem::error() const { return impl_->error; }
bool XrSystem::available() const { return impl_->session != XR_NULL_HANDLE; }
bool XrSystem::running() const { return impl_->session_running; }
bool XrSystem::focused() const { return impl_->session_running && impl_->state == XR_SESSION_STATE_FOCUSED; }
bool XrSystem::exitRequested() const { return impl_->exit_requested; }
const std::string& XrSystem::runtimeName() const { return impl_->runtime_name; }
const std::string& XrSystem::systemName() const { return impl_->system_name; }
VkExtent2D XrSystem::eyeExtent() const { return impl_->extent; }
VkFormat XrSystem::swapchainFormat() const { return impl_->format; }
bool XrSystem::frameBegun() const { return impl_->frame_begun; }
bool XrSystem::shouldRender() const { return impl_->frame_begun && impl_->frame_state.shouldRender == XR_TRUE; }
const EyeView& XrSystem::eye(int index) const { return impl_->eye_views[static_cast<std::size_t>(index != 0 ? 1 : 0)]; }
const Pose& XrSystem::head() const { return impl_->head; }
const Controller& XrSystem::controller(Hand hand) const { return impl_->controllers[static_cast<std::size_t>(hand)]; }
TrackingOrigin XrSystem::trackingOrigin() const { return impl_->origin; }
void XrSystem::setTrackingOrigin(TrackingOrigin origin) { impl_->origin = origin; }

bool XrSystem::beginFrame() {
    Impl& d = *impl_;
    if (d.session == XR_NULL_HANDLE) return false;
    d.endFrameNow();  // uno sin terminar (un frame en que no se dibujo)
    d.pollEvents();
    if (!d.session_running) {
        for (Controller& c : d.controllers) c = Controller{};
        return false;
    }
    d.frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    if (!d.ok(xrWaitFrame(d.session, &wait, &d.frame_state), "xrWaitFrame")) return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!d.ok(xrBeginFrame(d.session, &begin), "xrBeginFrame")) return false;
    d.frame_begun = true;

    const XrTime time = d.frame_state.predictedDisplayTime;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = d.appSpace();
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::uint32_t count = 0;
    for (XrView& v : d.views) v = XrView{XR_TYPE_VIEW};
    const bool located = XR_SUCCEEDED(xrLocateViews(d.session, &locate, &view_state, 2, &count, d.views.data())) && count == 2;
    const bool valid = located && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (located) {
        for (int i = 0; i < 2; ++i) {
            const XrView& v = d.views[static_cast<std::size_t>(i)];
            d.eye_views[static_cast<std::size_t>(i)].pose = toPose(v.pose, valid);
            d.eye_views[static_cast<std::size_t>(i)].fov = Fov{v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
        }
    }
    d.head = d.locate(d.view_space, time);
    d.updateControllers(time);
    return d.frame_state.shouldRender == XR_TRUE && located;
}

VkImage XrSystem::acquireEye(int eye) {
    Impl& d = *impl_;
    if (!shouldRender() || eye < 0 || eye > 1) return VK_NULL_HANDLE;
    Impl::EyeSwapchain& sc = d.eyes_sc[static_cast<std::size_t>(eye)];
    if (sc.handle == XR_NULL_HANDLE || sc.holding) return VK_NULL_HANDLE;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!d.ok(xrAcquireSwapchainImage(sc.handle, &acquire, &sc.acquired), "xrAcquireSwapchainImage")) return VK_NULL_HANDLE;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!d.ok(xrWaitSwapchainImage(sc.handle, &wait), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &release);
        return VK_NULL_HANDLE;
    }
    sc.holding = true;
    return sc.images[sc.acquired].image;
}

void XrSystem::releaseEye(int eye) {
    Impl& d = *impl_;
    if (eye < 0 || eye > 1) return;
    Impl::EyeSwapchain& sc = d.eyes_sc[static_cast<std::size_t>(eye)];
    if (!sc.holding) return;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    d.ok(xrReleaseSwapchainImage(sc.handle, &release), "xrReleaseSwapchainImage");
    sc.holding = false;
    sc.drawn = true;
}

void XrSystem::endFrame() { impl_->endFrameNow(); }

void XrSystem::vibrate(Hand hand, float amplitude, float seconds, float frequency) {
    Impl& d = *impl_;
    if (d.haptic == XR_NULL_HANDLE || !focused()) return;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = seconds <= 0.0f ? XR_MIN_HAPTIC_DURATION : static_cast<XrDuration>(seconds * 1e9);
    vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = d.haptic;
    info.subactionPath = d.hand_paths[static_cast<std::size_t>(hand)];
    xrApplyHapticFeedback(d.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

#else  // sin OpenXR (Android o CRAMION_XR=OFF)

struct XrSystem::Impl {
    std::string error = "El motor se compilo sin OpenXR";
    std::string empty;
    std::array<EyeView, 2> eyes{};
    Pose head;
    std::array<Controller, 2> controllers{};
    TrackingOrigin origin = TrackingOrigin::Floor;
};

XrSystem::XrSystem() : impl_(std::make_unique<Impl>()) {}
XrSystem::~XrSystem() = default;
bool XrSystem::compiled() { return false; }
bool XrSystem::createInstance(const char*) { return false; }
std::vector<std::string> XrSystem::requiredInstanceExtensions() const { return {}; }
std::vector<std::string> XrSystem::requiredDeviceExtensions() const { return {}; }
VkPhysicalDevice XrSystem::physicalDevice(VkInstance) const { return VK_NULL_HANDLE; }
bool XrSystem::createSession(VkInstance, VkPhysicalDevice, VkDevice, std::uint32_t, std::uint32_t) { return false; }
void XrSystem::shutdown() {}
const std::string& XrSystem::error() const { return impl_->error; }
bool XrSystem::available() const { return false; }
bool XrSystem::running() const { return false; }
bool XrSystem::focused() const { return false; }
bool XrSystem::exitRequested() const { return false; }
const std::string& XrSystem::runtimeName() const { return impl_->empty; }
const std::string& XrSystem::systemName() const { return impl_->empty; }
VkExtent2D XrSystem::eyeExtent() const { return {0, 0}; }
VkFormat XrSystem::swapchainFormat() const { return VK_FORMAT_UNDEFINED; }
bool XrSystem::beginFrame() { return false; }
bool XrSystem::frameBegun() const { return false; }
bool XrSystem::shouldRender() const { return false; }
const EyeView& XrSystem::eye(int index) const { return impl_->eyes[index != 0 ? 1 : 0]; }
const Pose& XrSystem::head() const { return impl_->head; }
const Controller& XrSystem::controller(Hand hand) const { return impl_->controllers[static_cast<std::size_t>(hand)]; }
VkImage XrSystem::acquireEye(int) { return VK_NULL_HANDLE; }
void XrSystem::releaseEye(int) {}
void XrSystem::endFrame() {}
void XrSystem::vibrate(Hand, float, float, float) {}
void XrSystem::setTrackingOrigin(TrackingOrigin origin) { impl_->origin = origin; }
TrackingOrigin XrSystem::trackingOrigin() const { return impl_->origin; }

#endif

}  // namespace cramion::xr
