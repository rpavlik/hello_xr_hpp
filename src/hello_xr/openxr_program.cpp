// Copyright (c) 2017-2020 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "pch.h"
#include "common.h"
#include "options.h"
#include "platformdata.h"
#include "platformplugin.h"
#include "graphicsplugin.h"
#include "openxr_program.h"
#include <common/xr_linear.h>
#include <array>
#include <cmath>

#include <openxr/openxr_handles.hpp>
#include <openxr/openxr_method_impls.hpp>

namespace {

#if !defined(XR_USE_PLATFORM_WIN32)
#define strcpy_s(dest, source) strncpy((dest), (source), sizeof(dest))
#endif

namespace Side {
const int LEFT = 0;
const int RIGHT = 1;
const int COUNT = 2;
}  // namespace Side

inline std::string GetXrVersionString(XrVersion ver) {
    return Fmt("%d.%d.%d", XR_VERSION_MAJOR(ver), XR_VERSION_MINOR(ver), XR_VERSION_PATCH(ver));
}

inline XrFormFactor GetXrFormFactor(const std::string& formFactorStr) {
    if (EqualsIgnoreCase(formFactorStr, "Hmd")) {
        return XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    }
    if (EqualsIgnoreCase(formFactorStr, "Handheld")) {
        return XR_FORM_FACTOR_HANDHELD_DISPLAY;
    }
    throw std::invalid_argument(Fmt("Unknown form factor '%s'", formFactorStr.c_str()));
}

inline XrViewConfigurationType GetXrViewConfigurationType(const std::string& viewConfigurationStr) {
    if (EqualsIgnoreCase(viewConfigurationStr, "Mono")) {
        return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    }
    if (EqualsIgnoreCase(viewConfigurationStr, "Stereo")) {
        return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    throw std::invalid_argument(Fmt("Unknown view configuration '%s'", viewConfigurationStr.c_str()));
}

inline XrEnvironmentBlendMode GetXrEnvironmentBlendMode(const std::string& environmentBlendModeStr) {
    if (EqualsIgnoreCase(environmentBlendModeStr, "Opaque")) {
        return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "Additive")) {
        return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "AlphaBlend")) {
        return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    }
    throw std::invalid_argument(Fmt("Unknown environment blend mode '%s'", environmentBlendModeStr.c_str()));
}

namespace Math {
namespace Pose {
XrPosef Identity() {
    XrPosef t{};
    t.orientation.w = 1;
    return t;
}

XrPosef Translation(const XrVector3f& translation) {
    XrPosef t = Identity();
    t.position = translation;
    return t;
}

XrPosef RotateCCWAboutYAxis(float radians, XrVector3f translation) {
    XrPosef t = Identity();
    t.orientation.x = 0.f;
    t.orientation.y = std::sin(radians * 0.5f);
    t.orientation.z = 0.f;
    t.orientation.w = std::cos(radians * 0.5f);
    t.position = translation;
    return t;
}
}  // namespace Pose
}  // namespace Math

inline xr::ReferenceSpaceCreateInfo GetXrReferenceSpaceCreateInfo(const std::string& referenceSpaceTypeStr) {
    xr::ReferenceSpaceCreateInfo referenceSpaceCreateInfo;
    if (EqualsIgnoreCase(referenceSpaceTypeStr, "View")) {
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::View;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "ViewFront")) {
        // Render head-locked 2m in front of device.
        referenceSpaceCreateInfo.poseInReferenceSpace = xr::Posef{{}, {0.f, 0.f, -2.f}};
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::View;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Local")) {
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Local;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Stage")) {
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeft")) {
        *put(referenceSpaceCreateInfo.poseInReferenceSpace) = Math::Pose::RotateCCWAboutYAxis(0.f, {-2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRight")) {
        *put(referenceSpaceCreateInfo.poseInReferenceSpace) = Math::Pose::RotateCCWAboutYAxis(0.f, {2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeftRotated")) {
        *put(referenceSpaceCreateInfo.poseInReferenceSpace) = Math::Pose::RotateCCWAboutYAxis(3.14f / 3.f, {-2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRightRotated")) {
        *put(referenceSpaceCreateInfo.poseInReferenceSpace) = Math::Pose::RotateCCWAboutYAxis(-3.14f / 3.f, {2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = xr::ReferenceSpaceType::Stage;
    } else {
        throw std::invalid_argument(Fmt("Unknown reference space type '%s'", referenceSpaceTypeStr.c_str()));
    }
    return referenceSpaceCreateInfo;
}

struct OpenXrProgram : IOpenXrProgram {
    OpenXrProgram(const std::shared_ptr<Options>& options, const std::shared_ptr<IPlatformPlugin>& platformPlugin,
                  const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin)
        : m_options(options), m_platformPlugin(platformPlugin), m_graphicsPlugin(graphicsPlugin) {}

    ~OpenXrProgram() override {
        if (m_input.actionSet != XR_NULL_HANDLE) {
            for (auto hand : {Side::LEFT, Side::RIGHT}) {
                xrDestroySpace(m_input.handSpace[hand]);
            }
            xrDestroyActionSet(m_input.actionSet);
        }

        for (Swapchain swapchain : m_swapchains) {
            xrDestroySwapchain(swapchain.handle);
        }

        for (XrSpace visualizedSpace : m_visualizedSpaces) {
            xrDestroySpace(visualizedSpace);
        }

        if (m_appSpace != XR_NULL_HANDLE) {
            xrDestroySpace(m_appSpace);
        }

        if (m_session != XR_NULL_HANDLE) {
            xrDestroySession(m_session);
        }

        if (m_instance != XR_NULL_HANDLE) {
            xrDestroyInstance(m_instance);
        }
    }

    static void LogLayersAndExtensions() {
        // Write out extension properties for a given layer.
        const auto logExtensions = [](const char* layerName, int indent = 0) {
            std::vector<xr::ExtensionProperties> extensions = xr::enumerateInstanceExtensionPropertiesToVector(layerName);

            const std::string indentStr(indent, ' ');
            Log::Write(Log::Level::Verbose, Fmt("%sAvailable Extensions: (%d)", indentStr.c_str(), extensions.size()));
            for (const XrExtensionProperties& extension : extensions) {
                Log::Write(Log::Level::Verbose, Fmt("%s  Name=%s SpecVersion=%d", indentStr.c_str(), extension.extensionName,
                                                    extension.extensionVersion));
            }
        };

        // Log non-layer extensions (layerName==nullptr).
        logExtensions(nullptr);

        // Log layers and any of their extensions.
        {
            std::vector<xr::ApiLayerProperties> layers = xr::enumerateApiLayerPropertiesToVector();
            Log::Write(Log::Level::Info, Fmt("Available Layers: (%d)", layers.size()));
            for (const XrApiLayerProperties& layer : layers) {
                Log::Write(Log::Level::Verbose,
                           Fmt("  Name=%s SpecVersion=%s LayerVersion=%d Description=%s", layer.layerName,
                               GetXrVersionString(layer.specVersion).c_str(), layer.layerVersion, layer.description));
                logExtensions(layer.layerName, 4);
            }
        }
    }

    void LogInstanceInfo() {
        CHECK(m_instance != XR_NULL_HANDLE);
        xr::InstanceProperties instanceProperties = xr::Instance(m_instance).getInstanceProperties();
        Log::Write(Log::Level::Info, Fmt("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
                                         to_string(instanceProperties.runtimeVersion).c_str()));
    }

    void CreateInstanceInternal() {
        CHECK(m_instance == XR_NULL_HANDLE);

        // Create union of extensions required by platform and graphics plugins.
        std::vector<const char*> extensions;

        // Transform platform and graphics extension std::strings to C strings.
        const std::vector<std::string> platformExtensions = m_platformPlugin->GetInstanceExtensions();
        std::transform(platformExtensions.begin(), platformExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });
        const std::vector<std::string> graphicsExtensions = m_graphicsPlugin->GetInstanceExtensions();
        std::transform(graphicsExtensions.begin(), graphicsExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });

        xr::InstanceCreateInfo createInfo{{},
                                          xr::ApplicationInfo{"HelloXR_Hpp", 0, nullptr, 0, xr::Version::current()},
                                          0,
                                          nullptr,
                                          (uint32_t)extensions.size(),
                                          extensions.data(),
                                          m_platformPlugin->GetInstanceCreateExtension()};

        // Here, we are just using OpenXR-Hpp to make creation of the structs easier.
        CHECK_XRCMD(xrCreateInstance(get(createInfo), &m_instance));
    }

    void CreateInstance() override {
        LogLayersAndExtensions();

        CreateInstanceInternal();

        LogInstanceInfo();
    }

    void LogViewConfigurations() {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        xr::Instance instance{m_instance};
        xr::SystemId systemId{m_systemId};

        std::vector<xr::ViewConfigurationType> viewConfigTypes = instance.enumerateViewConfigurationsToVector(systemId);

        Log::Write(Log::Level::Info, Fmt("Available View Configuration Types: (%d)", viewConfigTypes.size()));
        for (xr::ViewConfigurationType viewConfigType : viewConfigTypes) {
            Log::Write(Log::Level::Verbose, Fmt("  View Configuration Type: %s %s", to_string(viewConfigType).c_str(),
                                                get(viewConfigType) == m_viewConfigType ? "(Selected)" : ""));

            xr::ViewConfigurationProperties viewConfigProperties =
                instance.getViewConfigurationProperties(systemId, viewConfigType);

            Log::Write(Log::Level::Verbose,
                       Fmt("  View configuration FovMutable=%s", viewConfigProperties.fovMutable == XR_TRUE ? "True" : "False"));

            std::vector<xr::ViewConfigurationView> views =
                instance.enumerateViewConfigurationViewsToVector(systemId, viewConfigType);

            for (uint32_t i = 0; i < views.size(); i++) {
                const XrViewConfigurationView& view = views[i];

                Log::Write(Log::Level::Verbose,
                           Fmt("    View [%d]: Recommended Width=%d Height=%d SampleCount=%d", i, view.recommendedImageRectWidth,
                               view.recommendedImageRectHeight, view.recommendedSwapchainSampleCount));
                Log::Write(Log::Level::Verbose, Fmt("    View [%d]:     Maximum Width=%d Height=%d SampleCount=%d", i,
                                                    view.maxImageRectWidth, view.maxImageRectHeight, view.maxSwapchainSampleCount));
            }
            if (views.empty()) {
                Log::Write(Log::Level::Error, Fmt("Empty view configuration type"));
            }

            LogEnvironmentBlendMode(get(viewConfigType));
        }
    }

    void LogEnvironmentBlendMode(XrViewConfigurationType type) {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != 0);
        xr::Instance instance{m_instance};
        xr::SystemId systemId{m_systemId};

        std::vector<xr::EnvironmentBlendMode> blendModes =
            instance.enumerateEnvironmentBlendModesToVector(systemId, xr::ViewConfigurationType(type));
        CHECK(!blendModes.empty());

        Log::Write(Log::Level::Info, Fmt("Available Environment Blend Mode count : (%d)", blendModes.size()));

        bool blendModeFound = false;
        for (xr::EnvironmentBlendMode mode : blendModes) {
            const bool blendModeMatch = (get(mode) == m_environmentBlendMode);
            Log::Write(Log::Level::Info,
                       Fmt("Environment Blend Mode (%s) : %s", to_string(mode).c_str(), blendModeMatch ? "(Selected)" : ""));
            blendModeFound |= blendModeMatch;
        }
        CHECK(blendModeFound);
    }

    void InitializeSystem() override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId == XR_NULL_SYSTEM_ID);
        xr::Instance instance{m_instance};

        m_formFactor = GetXrFormFactor(m_options->FormFactor);
        m_viewConfigType = GetXrViewConfigurationType(m_options->ViewConfiguration);
        m_environmentBlendMode = GetXrEnvironmentBlendMode(m_options->EnvironmentBlendMode);

        xr::SystemId systemId = instance.getSystem(xr::SystemGetInfo{xr::FormFactor(m_formFactor)});
        m_systemId = get(systemId);

        Log::Write(Log::Level::Verbose, Fmt("Using system %d for form factor %s", m_systemId, to_string(m_formFactor)));
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        LogViewConfigurations();

        // The graphics API can initialize the graphics device now that the systemId and instance
        // handle are available.
        m_graphicsPlugin->InitializeDevice(m_instance, m_systemId);
    }

    void LogReferenceSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);
        xr::Session session{m_session};
        std::vector<xr::ReferenceSpaceType> spaces = session.enumerateReferenceSpacesToVector();

        Log::Write(Log::Level::Info, Fmt("Available reference spaces: %d", spaces.size()));
        for (xr::ReferenceSpaceType space : spaces) {
            Log::Write(Log::Level::Verbose, Fmt("  Name: %s", to_string(space).c_str()));
        }
    }

    struct InputState {
        XrActionSet actionSet{XR_NULL_HANDLE};
        XrAction grabAction{XR_NULL_HANDLE};
        XrAction poseAction{XR_NULL_HANDLE};
        XrAction vibrateAction{XR_NULL_HANDLE};
        XrAction quitAction{XR_NULL_HANDLE};
        std::array<XrPath, Side::COUNT> handSubactionPath;
        std::array<XrSpace, Side::COUNT> handSpace;
        std::array<float, Side::COUNT> handScale = {{1.0f, 1.0f}};
        std::array<XrBool32, Side::COUNT> handActive;
    };

    void InitializeActions() {
        CHECK(m_instance != XR_NULL_HANDLE);
        xr::Instance instance{m_instance};

        // Create an action set.
        xr::ActionSet actionSet = instance.createActionSet(xr::ActionSetCreateInfo{"gameplay", "Gameplay", 0});
        m_input.actionSet = get(actionSet);

        // Get the XrPath for the left and right hands - we will use them as subaction paths.
        m_input.handSubactionPath[Side::LEFT] = get(instance.stringToPath("/user/hand/left"));
        m_input.handSubactionPath[Side::RIGHT] = get(instance.stringToPath("/user/hand/right"));

        // Create actions.
        {
            // Create an input action for grabbing objects with the left and right hands.
            m_input.grabAction = get(actionSet.createAction(
                xr::ActionCreateInfo{"grab_object", xr::ActionType::FloatInput, uint32_t(m_input.handSubactionPath.size()),
                                     (const xr::Path*)m_input.handSubactionPath.data(), "Grab Object"}));

            // Create an input action getting the left and right hand poses.
            m_input.poseAction = get(actionSet.createAction(
                xr::ActionCreateInfo{"hand_pose", xr::ActionType::PoseInput, uint32_t(m_input.handSubactionPath.size()),
                                     (const xr::Path*)m_input.handSubactionPath.data(), "Hand Pose"}));

            // Create output actions for vibrating the left and right controller.
            m_input.vibrateAction = get(actionSet.createAction(
                xr::ActionCreateInfo{"vibrate_hand", xr::ActionType::VibrationOutput, uint32_t(m_input.handSubactionPath.size()),
                                     (const xr::Path*)m_input.handSubactionPath.data(), "Vibrate Hand"}));

            // Create input actions for quitting the session using the left and right controller.
            // Since it doesn't matter which hand did this, we do not specify subaction paths for it.
            // We will just suggest bindings for both hands, where possible.
            m_input.quitAction = get(actionSet.createAction(
                xr::ActionCreateInfo{"quit_session", xr::ActionType::BooleanInput, 0, nullptr, "Quit Session"}));
        }

        std::array<xr::Path, Side::COUNT> selectPath;
        std::array<xr::Path, Side::COUNT> squeezeValuePath;
        std::array<xr::Path, Side::COUNT> squeezeForcePath;
        std::array<xr::Path, Side::COUNT> squeezeClickPath;
        std::array<xr::Path, Side::COUNT> posePath;
        std::array<xr::Path, Side::COUNT> hapticPath;
        std::array<xr::Path, Side::COUNT> menuClickPath;
        std::array<xr::Path, Side::COUNT> bClickPath;
        std::array<xr::Path, Side::COUNT> triggerValuePath;
        selectPath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/select/click");
        selectPath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/select/click");
        squeezeValuePath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/squeeze/value");
        squeezeValuePath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/squeeze/value");
        squeezeForcePath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/squeeze/force");
        squeezeForcePath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/squeeze/force");
        squeezeClickPath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/squeeze/click");
        squeezeClickPath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/squeeze/click");
        posePath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/grip/pose");
        posePath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/grip/pose");
        hapticPath[Side::LEFT] = instance.stringToPath("/user/hand/left/output/haptic");
        hapticPath[Side::RIGHT] = instance.stringToPath("/user/hand/right/output/haptic");
        menuClickPath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/menu/click");
        menuClickPath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/menu/click");
        bClickPath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/b/click");
        bClickPath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/b/click");
        triggerValuePath[Side::LEFT] = instance.stringToPath("/user/hand/left/input/trigger/value");
        triggerValuePath[Side::RIGHT] = instance.stringToPath("/user/hand/right/input/trigger/value");

        // Suggest bindings for KHR Simple.
        {
            auto khrSimpleInteractionProfilePath = instance.stringToPath("/interaction_profiles/khr/simple_controller");
            std::vector<xr::ActionSuggestedBinding> bindings{
                {// Fall back to a click input for the grab action.
                 xr::ActionSuggestedBinding{xr::Action(m_input.grabAction), selectPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.grabAction), selectPath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.poseAction), posePath[Side::LEFT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.poseAction), posePath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.quitAction), menuClickPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.quitAction), menuClickPath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.vibrateAction), hapticPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{xr::Action(m_input.vibrateAction), hapticPath[Side::RIGHT]}}};

            instance.suggestInteractionProfileBindings(
                {khrSimpleInteractionProfilePath, uint32_t(bindings.size()), bindings.data()});
        }
#if 0
        // Suggest bindings for the Oculus Touch.
        {
            XrPath oculusTouchInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &oculusTouchInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeValuePath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeValuePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        // Suggest bindings for the Vive Controller.
        {
            XrPath viveControllerInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/htc/vive_controller", &viveControllerInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, triggerValuePath[Side::LEFT]},
                                                            {m_input.grabAction, triggerValuePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = viveControllerInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        // Suggest bindings for the Valve Index Controller.
        {
            XrPath indexControllerInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &indexControllerInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeForcePath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeForcePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, bClickPath[Side::LEFT]},
                                                            {m_input.quitAction, bClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = indexControllerInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        // Suggest bindings for the Microsoft Mixed Reality Motion Controller.
        {
            XrPath microsoftMixedRealityInteractionProfilePath;
            CHECK_XRCMD(xrStringToPath(m_instance, "/interaction_profiles/microsoft/motion_controller",
                                       &microsoftMixedRealityInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeClickPath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeClickPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = microsoftMixedRealityInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
#endif
        xr::Session session{m_session};
        m_input.handSpace[Side::LEFT] = get(session.createActionSpace(
            xr::ActionSpaceCreateInfo{xr::Action{m_input.poseAction}, xr::Path(m_input.handSubactionPath[Side::LEFT]), {}}));
        m_input.handSpace[Side::RIGHT] = get(session.createActionSpace(
            xr::ActionSpaceCreateInfo{xr::Action{m_input.poseAction}, xr::Path(m_input.handSubactionPath[Side::RIGHT]), {}}));

        session.attachSessionActionSets({1, &actionSet});
    }

    void CreateVisualizedSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);
        xr::Session session{m_session};

        std::string visualizedSpaces[] = {"ViewFront",        "Local", "Stage", "StageLeft", "StageRight", "StageLeftRotated",
                                          "StageRightRotated"};

        for (const auto& visualizedSpace : visualizedSpaces) {
            xr::Space space;
            xr::Result res = session.createReferenceSpace(GetXrReferenceSpaceCreateInfo(visualizedSpace), space);
            if (succeeded(res)) {
                m_visualizedSpaces.push_back(space);
            } else {
                Log::Write(Log::Level::Warning,
                           Fmt("Failed to create reference space %s with error %d", visualizedSpace.c_str(), res));
            }
        }
    }

    void InitializeSession() override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_session == XR_NULL_HANDLE);

        xr::Instance instance{m_instance};
        {
            Log::Write(Log::Level::Verbose, Fmt("Creating session..."));

            m_session = get(instance.createSession(
                xr::SessionCreateInfo{{}, xr::SystemId(m_systemId), m_graphicsPlugin->GetGraphicsBinding()}));
        }

        LogReferenceSpaces();
        InitializeActions();
        CreateVisualizedSpaces();

        xr::Session session{m_session};
        m_appSpace = get(session.createReferenceSpace(GetXrReferenceSpaceCreateInfo(m_options->AppSpace)));
    }

    void CreateSwapchains() override {
        CHECK(m_session != XR_NULL_HANDLE);
        CHECK(m_swapchains.empty());
        CHECK(m_configViews.empty());
        xr::Instance instance{m_instance};
        xr::SystemId systemId{m_systemId};
        xr::Session session{m_session};

        // Read graphics properties for preferred swapchain length and logging.
        xr::SystemProperties systemProperties = instance.getSystemProperties(systemId);

        // Log system properties.
        Log::Write(Log::Level::Info,
                   Fmt("System Properties: Name=%s VendorId=%d", systemProperties.systemName, systemProperties.vendorId));
        Log::Write(Log::Level::Info, Fmt("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
                                         systemProperties.graphicsProperties.maxSwapchainImageWidth,
                                         systemProperties.graphicsProperties.maxSwapchainImageHeight,
                                         systemProperties.graphicsProperties.maxLayerCount));
        Log::Write(Log::Level::Info, Fmt("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
                                         systemProperties.trackingProperties.orientationTracking == XR_TRUE ? "True" : "False",
                                         systemProperties.trackingProperties.positionTracking == XR_TRUE ? "True" : "False"));

        // Note: No other view configurations exist at the time this code was written. If this
        // condition is not met, the project will need to be audited to see how support should be
        // added.
        CHECK_MSG(m_viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "Unsupported view configuration type");

        // Query and cache view configuration views.
        m_configViews = instance.enumerateViewConfigurationViewsToVector(systemId, xr::ViewConfigurationType(m_viewConfigType));

        // Create and cache view buffer for xrLocateViews later.
        m_views.resize(m_configViews.size(), {XR_TYPE_VIEW});

        // Create the swapchain and get the images.
        if (!m_configViews.empty()) {
            // Select a swapchain format.
            std::vector<int64_t> swapchainFormats = session.enumerateSwapchainFormatsToVector();
            m_colorSwapchainFormat = m_graphicsPlugin->SelectColorSwapchainFormat(swapchainFormats);

            // Print swapchain formats and the selected one.
            {
                std::string swapchainFormatsString;
                for (int64_t format : swapchainFormats) {
                    const bool selected = format == m_colorSwapchainFormat;
                    swapchainFormatsString += " ";
                    if (selected) {
                        swapchainFormatsString += "[";
                    }
                    swapchainFormatsString += std::to_string(format);
                    if (selected) {
                        swapchainFormatsString += "]";
                    }
                }
                Log::Write(Log::Level::Verbose, Fmt("Swapchain Formats: %s", swapchainFormatsString.c_str()));
            }
            const auto viewCount = m_configViews.size();
            // Create a swapchain for each view.
            for (uint32_t i = 0; i < viewCount; i++) {
                const xr::ViewConfigurationView& vp = m_configViews[i];
                Log::Write(Log::Level::Info,
                           Fmt("Creating swapchain for view %d with dimensions Width=%d Height=%d SampleCount=%d", i,
                               vp.recommendedImageRectWidth, vp.recommendedImageRectHeight, vp.recommendedSwapchainSampleCount));

                // Create the swapchain.
                xr::SwapchainCreateInfo swapchainCreateInfo;
                swapchainCreateInfo.arraySize = 1;
                swapchainCreateInfo.format = m_colorSwapchainFormat;
                swapchainCreateInfo.width = vp.recommendedImageRectWidth;
                swapchainCreateInfo.height = vp.recommendedImageRectHeight;
                swapchainCreateInfo.mipCount = 1;
                swapchainCreateInfo.faceCount = 1;
                swapchainCreateInfo.sampleCount = m_graphicsPlugin->GetSupportedSwapchainSampleCount(vp);
                swapchainCreateInfo.usageFlags = xr::SwapchainUsageFlagBits::Sampled | xr::SwapchainUsageFlagBits::ColorAttachment;
                Swapchain swapchain;
                swapchain.width = swapchainCreateInfo.width;
                swapchain.height = swapchainCreateInfo.height;
                swapchain.handle = get(session.createSwapchain(swapchainCreateInfo));

                m_swapchains.push_back(swapchain);

                uint32_t imageCount;
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr));
                // XXX This should really just return XrSwapchainImageBaseHeader*
                std::vector<XrSwapchainImageBaseHeader*> swapchainImages =
                    m_graphicsPlugin->AllocateSwapchainImageStructs(imageCount, swapchainCreateInfo);
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, swapchainImages[0]));

                m_swapchainImages.insert(std::make_pair(swapchain.handle, std::move(swapchainImages)));
            }
        }
    }

    // Return event if one is available, otherwise return null.
    const XrEventDataBaseHeader* TryReadNextEvent() {
        m_eventDataBuffer = {};
        xr::Result res = xr::Instance(m_instance).pollEvent(m_eventDataBuffer);
        if (unqualifiedSuccess(res)) {
            XrEventDataBaseHeader* baseHeader = reinterpret_cast<XrEventDataBaseHeader*>(&m_eventDataBuffer);
            if (m_eventDataBuffer.type == xr::StructureType::EventDataEventsLost) {
                const xr::EventDataEventsLost* const eventsLost = reinterpret_cast<const xr::EventDataEventsLost*>(baseHeader);
                Log::Write(Log::Level::Warning, Fmt("%d events lost", eventsLost->lostEventCount));
            }

            return reinterpret_cast<const XrEventDataBaseHeader*>(baseHeader);
        }
        if (res == xr::Result::EventUnavailable) {
            return nullptr;
        }
        THROW_XR(get(res), "xrPollEvent");
    }

    void PollEvents(bool* exitRenderLoop, bool* requestRestart) override {
        *exitRenderLoop = *requestRestart = false;

        // Process all pending messages.
        while (const XrEventDataBaseHeader* event = TryReadNextEvent()) {
            switch (event->type) {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    const auto& instanceLossPending = *reinterpret_cast<const XrEventDataInstanceLossPending*>(event);
                    Log::Write(Log::Level::Warning, Fmt("XrEventDataInstanceLossPending by %lld", instanceLossPending.lossTime));
                    *exitRenderLoop = true;
                    *requestRestart = true;
                    return;
                }
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto sessionStateChangedEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(event);
                    HandleSessionStateChangedEvent(sessionStateChangedEvent, exitRenderLoop, requestRestart);
                    break;
                }
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    LogActionSourceName(m_input.grabAction, "Grab");
                    LogActionSourceName(m_input.quitAction, "Quit");
                    LogActionSourceName(m_input.poseAction, "Pose");
                    LogActionSourceName(m_input.vibrateAction, "Vibrate");
                    break;
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                default: {
                    Log::Write(Log::Level::Verbose, Fmt("Ignoring event type %d", event->type));
                    break;
                }
            }
        }
    }

    void HandleSessionStateChangedEvent(const XrEventDataSessionStateChanged& stateChangedEvent, bool* exitRenderLoop,
                                        bool* requestRestart) {
        const XrSessionState oldState = m_sessionState;
        m_sessionState = stateChangedEvent.state;

        Log::Write(Log::Level::Info, Fmt("XrEventDataSessionStateChanged: state %s->%s session=%lld time=%lld", to_string(oldState),
                                         to_string(m_sessionState), stateChangedEvent.session, stateChangedEvent.time));

        if ((stateChangedEvent.session != XR_NULL_HANDLE) && (stateChangedEvent.session != m_session)) {
            Log::Write(Log::Level::Error, "XrEventDataSessionStateChanged for unknown session");
            return;
        }

        switch (m_sessionState) {
            case XR_SESSION_STATE_READY: {
                CHECK(m_session != XR_NULL_HANDLE);
                XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                sessionBeginInfo.primaryViewConfigurationType = m_viewConfigType;
                CHECK_XRCMD(xrBeginSession(m_session, &sessionBeginInfo));
                m_sessionRunning = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING: {
                CHECK(m_session != XR_NULL_HANDLE);
                m_sessionRunning = false;
                CHECK_XRCMD(xrEndSession(m_session))
                break;
            }
            case XR_SESSION_STATE_EXITING: {
                *exitRenderLoop = true;
                // Do not attempt to restart because user closed this session.
                *requestRestart = false;
                break;
            }
            case XR_SESSION_STATE_LOSS_PENDING: {
                *exitRenderLoop = true;
                // Poll for a new instance.
                *requestRestart = true;
                break;
            }
            default:
                break;
        }
    }

    void LogActionSourceName(XrAction action, const std::string& actionName) const {
        XrBoundSourcesForActionEnumerateInfo getInfo = {XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        getInfo.action = action;
        uint32_t pathCount = 0;
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, 0, &pathCount, nullptr));
        std::vector<XrPath> paths(pathCount);
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, uint32_t(paths.size()), &pathCount, paths.data()));

        std::string sourceName;
        for (uint32_t i = 0; i < pathCount; ++i) {
            std::string grabSource = xr::Session(m_session).getInputSourceLocalizedName(
                {xr::Path(paths[i]), xr::InputSourceLocalizedNameFlagBits::AllBits});
            if (grabSource.empty()) {
                continue;
            }
            if (!sourceName.empty()) {
                sourceName += " and ";
            }
            sourceName += "'";
            sourceName += grabSource;
            sourceName += "'";
        }

        Log::Write(Log::Level::Info,
                   Fmt("%s action is bound to %s", actionName.c_str(), ((!sourceName.empty()) ? sourceName.c_str() : "nothing")));
    }

    bool IsSessionRunning() const override { return m_sessionRunning; }

    bool IsSessionFocused() const override { return m_sessionState == XR_SESSION_STATE_FOCUSED; }

    void PollActions() override {
        m_input.handActive = {XR_FALSE, XR_FALSE};

        // Sync actions
        const XrActiveActionSet activeActionSet{m_input.actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeActionSet;
        CHECK_XRCMD(xrSyncActions(m_session, &syncInfo));

        // Get pose and grab action state and start haptic vibrate when hand is 90% squeezed.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = m_input.grabAction;
            getInfo.subactionPath = m_input.handSubactionPath[hand];

            XrActionStateFloat grabValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &grabValue));
            if (grabValue.isActive == XR_TRUE) {
                // Scale the rendered hand by 1.0f (open) to 0.5f (fully squeezed).
                m_input.handScale[hand] = 1.0f - 0.5f * grabValue.currentState;
                if (grabValue.currentState > 0.9f) {
                    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                    vibration.amplitude = 0.5;
                    vibration.duration = XR_MIN_HAPTIC_DURATION;
                    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

                    XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
                    hapticActionInfo.action = m_input.vibrateAction;
                    hapticActionInfo.subactionPath = m_input.handSubactionPath[hand];
                    CHECK_XRCMD(xrApplyHapticFeedback(m_session, &hapticActionInfo, (XrHapticBaseHeader*)&vibration));
                }
            }

            getInfo.action = m_input.poseAction;
            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            CHECK_XRCMD(xrGetActionStatePose(m_session, &getInfo, &poseState));
            m_input.handActive[hand] = poseState.isActive;
        }

        // There were no subaction paths specified for the quit action, because we don't care which hand did it.
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.quitAction, XR_NULL_PATH};
        XrActionStateBoolean quitValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &quitValue));
        if ((quitValue.isActive == XR_TRUE) && (quitValue.changedSinceLastSync == XR_TRUE) && (quitValue.currentState == XR_TRUE)) {
            CHECK_XRCMD(xrRequestExitSession(m_session));
        }
    }

    void RenderFrame() override {
        CHECK(m_session != XR_NULL_HANDLE);

        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XRCMD(xrWaitFrame(m_session, &frameWaitInfo, &frameState));

        XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XRCMD(xrBeginFrame(m_session, &frameBeginInfo));

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projectionLayerViews;
        if (frameState.shouldRender == XR_TRUE) {
            if (RenderLayer(frameState.predictedDisplayTime, projectionLayerViews, layer)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
            }
        }

        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_environmentBlendMode;
        frameEndInfo.layerCount = (uint32_t)layers.size();
        frameEndInfo.layers = layers.data();
        CHECK_XRCMD(xrEndFrame(m_session, &frameEndInfo));
    }

    bool RenderLayer(XrTime predictedDisplayTime, std::vector<XrCompositionLayerProjectionView>& projectionLayerViews,
                     XrCompositionLayerProjection& layer) {
        XrResult res;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCapacityInput = (uint32_t)m_views.size();
        uint32_t viewCountOutput;

        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = m_viewConfigType;
        viewLocateInfo.displayTime = predictedDisplayTime;
        viewLocateInfo.space = m_appSpace;

        res = xrLocateViews(m_session, &viewLocateInfo, &viewState, viewCapacityInput, &viewCountOutput, m_views.data());
        CHECK_XRRESULT(res, "xrLocateViews");
        if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            return false;  // There is no valid tracking poses for the views.
        }

        CHECK(viewCountOutput == viewCapacityInput);
        CHECK(viewCountOutput == m_configViews.size());
        CHECK(viewCountOutput == m_swapchains.size());

        projectionLayerViews.resize(viewCountOutput);

        // For each locatable space that we want to visualize, render a 25cm cube.
        std::vector<Cube> cubes;

        for (XrSpace visualizedSpace : m_visualizedSpaces) {
            XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(visualizedSpace, m_appSpace, predictedDisplayTime, &spaceLocation);
            CHECK_XRRESULT(res, "xrLocateSpace");
            if (XR_UNQUALIFIED_SUCCESS(res)) {
                if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                    (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                    cubes.push_back(Cube{spaceLocation.pose, {0.25f, 0.25f, 0.25f}});
                }
            } else {
                Log::Write(Log::Level::Verbose, Fmt("Unable to locate a visualized reference space in app space: %d", res));
            }
        }

        // Render a 10cm cube scaled by grabAction for each hand. Note renderHand will only be
        // true when the application has focus.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(m_input.handSpace[hand], m_appSpace, predictedDisplayTime, &spaceLocation);
            CHECK_XRRESULT(res, "xrLocateSpace");
            if (XR_UNQUALIFIED_SUCCESS(res)) {
                if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                    (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                    float scale = 0.1f * m_input.handScale[hand];
                    cubes.push_back(Cube{spaceLocation.pose, {scale, scale, scale}});
                }
            } else {
                // Tracking loss is expected when the hand is not active so only log a message
                // if the hand is active.
                if (m_input.handActive[hand] == XR_TRUE) {
                    const char* handName[] = {"left", "right"};
                    Log::Write(Log::Level::Verbose,
                               Fmt("Unable to locate %s hand action space in app space: %d", handName[hand], res));
                }
            }
        }

        // Render view to the appropriate part of the swapchain image.
        for (uint32_t i = 0; i < viewCountOutput; i++) {
            // Each view has a separate swapchain which is acquired, rendered to, and released.
            const Swapchain viewSwapchain = m_swapchains[i];

            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            uint32_t swapchainImageIndex;
            CHECK_XRCMD(xrAcquireSwapchainImage(viewSwapchain.handle, &acquireInfo, &swapchainImageIndex));

            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            CHECK_XRCMD(xrWaitSwapchainImage(viewSwapchain.handle, &waitInfo));

            projectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projectionLayerViews[i].pose = m_views[i].pose;
            projectionLayerViews[i].fov = m_views[i].fov;
            projectionLayerViews[i].subImage.swapchain = viewSwapchain.handle;
            projectionLayerViews[i].subImage.imageRect.offset = {0, 0};
            projectionLayerViews[i].subImage.imageRect.extent = {viewSwapchain.width, viewSwapchain.height};

            const XrSwapchainImageBaseHeader* const swapchainImage = m_swapchainImages[viewSwapchain.handle][swapchainImageIndex];
            m_graphicsPlugin->RenderView(projectionLayerViews[i], swapchainImage, m_colorSwapchainFormat, cubes);

            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK_XRCMD(xrReleaseSwapchainImage(viewSwapchain.handle, &releaseInfo));
        }

        layer.space = m_appSpace;
        layer.viewCount = (uint32_t)projectionLayerViews.size();
        layer.views = projectionLayerViews.data();
        return true;
    }

   private:
    const std::shared_ptr<Options> m_options;
    std::shared_ptr<IPlatformPlugin> m_platformPlugin;
    std::shared_ptr<IGraphicsPlugin> m_graphicsPlugin;
    XrInstance m_instance{XR_NULL_HANDLE};
    XrSession m_session{XR_NULL_HANDLE};
    XrSpace m_appSpace{XR_NULL_HANDLE};
    XrFormFactor m_formFactor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrViewConfigurationType m_viewConfigType{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrEnvironmentBlendMode m_environmentBlendMode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

    std::vector<xr::ViewConfigurationView> m_configViews;
    std::vector<Swapchain> m_swapchains;
    std::map<XrSwapchain, std::vector<XrSwapchainImageBaseHeader*>> m_swapchainImages;
    std::vector<XrView> m_views;
    int64_t m_colorSwapchainFormat{-1};

    std::vector<XrSpace> m_visualizedSpaces;

    // Application's current lifecycle state according to the runtime
    XrSessionState m_sessionState{XR_SESSION_STATE_UNKNOWN};
    bool m_sessionRunning{false};

    xr::EventDataBuffer m_eventDataBuffer;
    InputState m_input;
};
}  // namespace

std::shared_ptr<IOpenXrProgram> CreateOpenXrProgram(const std::shared_ptr<Options>& options,
                                                    const std::shared_ptr<IPlatformPlugin>& platformPlugin,
                                                    const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin) {
    return std::make_shared<OpenXrProgram>(options, platformPlugin, graphicsPlugin);
}
