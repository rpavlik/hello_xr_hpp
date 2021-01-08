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

inline xr::FormFactor GetXrFormFactor(const std::string& formFactorStr) {
    if (EqualsIgnoreCase(formFactorStr, "Hmd")) {
        return xr::FormFactor::HeadMountedDisplay;
    }
    if (EqualsIgnoreCase(formFactorStr, "Handheld")) {
        return xr::FormFactor::HandheldDisplay;
    }
    throw std::invalid_argument(Fmt("Unknown form factor '%s'", formFactorStr.c_str()));
}

inline xr::ViewConfigurationType GetXrViewConfigurationType(const std::string& viewConfigurationStr) {
    if (EqualsIgnoreCase(viewConfigurationStr, "Mono")) {
        return xr::ViewConfigurationType::PrimaryMono;
    }
    if (EqualsIgnoreCase(viewConfigurationStr, "Stereo")) {
        return xr::ViewConfigurationType::PrimaryStereo;
    }
    throw std::invalid_argument(Fmt("Unknown view configuration '%s'", viewConfigurationStr.c_str()));
}

inline xr::EnvironmentBlendMode GetXrEnvironmentBlendMode(const std::string& environmentBlendModeStr) {
    if (EqualsIgnoreCase(environmentBlendModeStr, "Opaque")) {
        return xr::EnvironmentBlendMode::Opaque;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "Additive")) {
        return xr::EnvironmentBlendMode::Additive;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "AlphaBlend")) {
        return xr::EnvironmentBlendMode::AlphaBlend;
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
        if (m_input.actionSet) {
            for (auto hand : {Side::LEFT, Side::RIGHT}) {
                m_input.handSpace[hand].destroy();
            }
            m_input.actionSet.destroy();
        }

        for (Swapchain& swapchain : m_swapchains) {
            xrDestroySwapchain(swapchain.handle);
        }

        for (xr::Space& visualizedSpace : m_visualizedSpaces) {
            visualizedSpace.destroy();
        }

        if (m_appSpace) {
            m_appSpace.destroy();
        }

        if (m_session) {
            m_session.destroy();
        }

        if (m_instance) {
            m_instance.destroy();
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
        CHECK(m_instance);
        xr::InstanceProperties instanceProperties = m_instance.getInstanceProperties();
        Log::Write(Log::Level::Info, Fmt("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
                                         to_string(instanceProperties.runtimeVersion).c_str()));
    }

    void CreateInstanceInternal() {
        CHECK(!m_instance);

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
        CHECK_XRCMD(xrCreateInstance(get(createInfo), put(m_instance)));
#if 0
        // We could have done
        m_instance = xr::createInstance(createInfo);
#endif
    }

    void CreateInstance() override {
        LogLayersAndExtensions();

        CreateInstanceInternal();

        LogInstanceInfo();
    }

    void LogViewConfigurations() {
        CHECK(m_instance);
        CHECK(m_systemId);

        std::vector<xr::ViewConfigurationType> viewConfigTypes = m_instance.enumerateViewConfigurationsToVector(m_systemId);

        Log::Write(Log::Level::Info, Fmt("Available View Configuration Types: (%d)", viewConfigTypes.size()));
        for (xr::ViewConfigurationType viewConfigType : viewConfigTypes) {
            Log::Write(Log::Level::Verbose, Fmt("  View Configuration Type: %s %s", to_string(viewConfigType).c_str(),
                                                viewConfigType == m_viewConfigType ? "(Selected)" : ""));

            xr::ViewConfigurationProperties viewConfigProperties =
                m_instance.getViewConfigurationProperties(m_systemId, viewConfigType);

            Log::Write(Log::Level::Verbose,
                       Fmt("  View configuration FovMutable=%s", viewConfigProperties.fovMutable == XR_TRUE ? "True" : "False"));

            std::vector<xr::ViewConfigurationView> views =
                m_instance.enumerateViewConfigurationViewsToVector(m_systemId, viewConfigType);

            for (uint32_t i = 0; i < views.size(); i++) {
                const xr::ViewConfigurationView& view = views[i];

                Log::Write(Log::Level::Verbose,
                           Fmt("    View [%d]: Recommended Width=%d Height=%d SampleCount=%d", i, view.recommendedImageRectWidth,
                               view.recommendedImageRectHeight, view.recommendedSwapchainSampleCount));
                Log::Write(Log::Level::Verbose, Fmt("    View [%d]:     Maximum Width=%d Height=%d SampleCount=%d", i,
                                                    view.maxImageRectWidth, view.maxImageRectHeight, view.maxSwapchainSampleCount));
            }
            if (views.empty()) {
                Log::Write(Log::Level::Error, Fmt("Empty view configuration type"));
            }

            LogEnvironmentBlendMode(viewConfigType);
        }
    }

    void LogEnvironmentBlendMode(xr::ViewConfigurationType type) {
        CHECK(m_instance);
        CHECK(m_systemId);

        std::vector<xr::EnvironmentBlendMode> blendModes = m_instance.enumerateEnvironmentBlendModesToVector(m_systemId, type);
        CHECK(!blendModes.empty());

        Log::Write(Log::Level::Info, Fmt("Available Environment Blend Mode count : (%d)", blendModes.size()));

        bool blendModeFound = false;
        for (xr::EnvironmentBlendMode mode : blendModes) {
            const bool blendModeMatch = (mode == m_environmentBlendMode);
            Log::Write(Log::Level::Info,
                       Fmt("Environment Blend Mode (%s) : %s", to_string(mode).c_str(), blendModeMatch ? "(Selected)" : ""));
            blendModeFound |= blendModeMatch;
        }
        CHECK(blendModeFound);
    }

    void InitializeSystem() override {
        CHECK(m_instance);
        CHECK(!m_systemId);

        m_formFactor = GetXrFormFactor(m_options->FormFactor);
        m_viewConfigType = GetXrViewConfigurationType(m_options->ViewConfiguration);
        m_environmentBlendMode = GetXrEnvironmentBlendMode(m_options->EnvironmentBlendMode);

        m_systemId = m_instance.getSystem(xr::SystemGetInfo{m_formFactor});

        Log::Write(Log::Level::Verbose,
                   Fmt("Using system %d for form factor %s", get(m_systemId), to_string(m_formFactor).c_str()));
        CHECK(m_instance);
        CHECK(m_systemId);

        LogViewConfigurations();

        // The graphics API can initialize the graphics device now that the systemId and instance
        // handle are available.
        m_graphicsPlugin->InitializeDevice(get(m_instance), get(m_systemId));
    }

    void LogReferenceSpaces() {
        CHECK(m_session);
        std::vector<xr::ReferenceSpaceType> spaces = m_session.enumerateReferenceSpacesToVector();

        Log::Write(Log::Level::Info, Fmt("Available reference spaces: %d", spaces.size()));
        for (xr::ReferenceSpaceType space : spaces) {
            Log::Write(Log::Level::Verbose, Fmt("  Name: %s", to_string(space).c_str()));
        }
    }

    struct InputState {
        xr::ActionSet actionSet;
        xr::Action grabAction;
        xr::Action poseAction;
        xr::Action vibrateAction;
        xr::Action quitAction;
        std::array<xr::Path, Side::COUNT> handSubactionPath;
        std::array<xr::Space, Side::COUNT> handSpace;
        std::array<float, Side::COUNT> handScale = {{1.0f, 1.0f}};
        std::array<XrBool32, Side::COUNT> handActive;
    };

    void InitializeActions() {
        CHECK(m_instance);

        // Create an action set.
        xr::ActionSet actionSet = m_instance.createActionSet({"gameplay", "Gameplay", 0});
        m_input.actionSet = actionSet;

        // Get the xr::Path for the left and right hands - we will use them as subaction paths.
        m_input.handSubactionPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left");
        m_input.handSubactionPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right");

        // Create actions.
        {
            // Create an input action for grabbing objects with the left and right hands.
            m_input.grabAction = actionSet.createAction(xr::ActionCreateInfo{"grab_object", xr::ActionType::FloatInput,
                                                                             uint32_t(m_input.handSubactionPath.size()),
                                                                             m_input.handSubactionPath.data(), "Grab Object"});

            // Create an input action getting the left and right hand poses.
            // Note that most of the time the name of the struct is optional in calls like this, so we removed it here.
            m_input.poseAction =
                actionSet.createAction({"hand_pose", xr::ActionType::PoseInput, uint32_t(m_input.handSubactionPath.size()),
                                        m_input.handSubactionPath.data(), "Hand Pose"});

            // Create output actions for vibrating the left and right controller.
            m_input.vibrateAction =
                actionSet.createAction({"vibrate_hand", xr::ActionType::VibrationOutput, uint32_t(m_input.handSubactionPath.size()),
                                        m_input.handSubactionPath.data(), "Vibrate Hand"});

            // Create input actions for quitting the session using the left and right controller.
            // Since it doesn't matter which hand did this, we do not specify subaction paths for it.
            // We will just suggest bindings for both hands, where possible.
            m_input.quitAction = actionSet.createAction({"quit_session", xr::ActionType::BooleanInput, 0, nullptr, "Quit Session"});
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
        selectPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/select/click");
        selectPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/select/click");
        squeezeValuePath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/squeeze/value");
        squeezeValuePath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/squeeze/value");
        squeezeForcePath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/squeeze/force");
        squeezeForcePath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/squeeze/force");
        squeezeClickPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/squeeze/click");
        squeezeClickPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/squeeze/click");
        posePath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/grip/pose");
        posePath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/grip/pose");
        hapticPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/output/haptic");
        hapticPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/output/haptic");
        menuClickPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/menu/click");
        menuClickPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/menu/click");
        bClickPath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/b/click");
        bClickPath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/b/click");
        triggerValuePath[Side::LEFT] = m_instance.stringToPath("/user/hand/left/input/trigger/value");
        triggerValuePath[Side::RIGHT] = m_instance.stringToPath("/user/hand/right/input/trigger/value");

        // Suggest bindings for KHR Simple.
        {
            auto khrSimpleInteractionProfilePath = m_instance.stringToPath("/interaction_profiles/khr/simple_controller");
            std::vector<xr::ActionSuggestedBinding> bindings{
                {// Fall back to a click input for the grab action.
                 xr::ActionSuggestedBinding{m_input.grabAction, selectPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{m_input.grabAction, selectPath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{m_input.poseAction, posePath[Side::LEFT]},
                 xr::ActionSuggestedBinding{m_input.poseAction, posePath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{m_input.quitAction, menuClickPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{m_input.quitAction, menuClickPath[Side::RIGHT]},
                 xr::ActionSuggestedBinding{m_input.vibrateAction, hapticPath[Side::LEFT]},
                 xr::ActionSuggestedBinding{m_input.vibrateAction, hapticPath[Side::RIGHT]}}};

            m_instance.suggestInteractionProfileBindings(
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
        m_input.handSpace[Side::LEFT] =
            m_session.createActionSpace(xr::ActionSpaceCreateInfo{m_input.poseAction, m_input.handSubactionPath[Side::LEFT], {}});
        m_input.handSpace[Side::RIGHT] =
            m_session.createActionSpace(xr::ActionSpaceCreateInfo{m_input.poseAction, m_input.handSubactionPath[Side::RIGHT], {}});

        m_session.attachSessionActionSets({1, &actionSet});
    }

    void CreateVisualizedSpaces() {
        CHECK(m_session);

        std::string visualizedSpaces[] = {"ViewFront",        "Local", "Stage", "StageLeft", "StageRight", "StageLeftRotated",
                                          "StageRightRotated"};

        for (const auto& visualizedSpace : visualizedSpaces) {
            // Here we are using the "simple" style wrapper because we actually want to get a result code,
            // not an exception
            xr::Space space;
            xr::Result res = m_session.createReferenceSpace(GetXrReferenceSpaceCreateInfo(visualizedSpace), space);
            if (succeeded(res)) {
                m_visualizedSpaces.push_back(space);
            } else {
                Log::Write(Log::Level::Warning,
                           Fmt("Failed to create reference space %s with error %d", visualizedSpace.c_str(), res));
            }
        }
    }

    void InitializeSession() override {
        CHECK(m_instance);
        CHECK(!m_session);

        {
            Log::Write(Log::Level::Verbose, Fmt("Creating session..."));

            m_session = m_instance.createSession(xr::SessionCreateInfo{{}, m_systemId, m_graphicsPlugin->GetGraphicsBinding()});
        }

        LogReferenceSpaces();
        InitializeActions();
        CreateVisualizedSpaces();

        m_appSpace = m_session.createReferenceSpace(GetXrReferenceSpaceCreateInfo(m_options->AppSpace));
    }

    void CreateSwapchains() override {
        CHECK(m_session);
        CHECK(m_swapchains.empty());
        CHECK(m_configViews.empty());

        // Read graphics properties for preferred swapchain length and logging.
        xr::SystemProperties systemProperties = m_instance.getSystemProperties(m_systemId);

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
        CHECK_MSG(m_viewConfigType == xr::ViewConfigurationType::PrimaryStereo, "Unsupported view configuration type");

        // Query and cache view configuration views.
        m_configViews = m_instance.enumerateViewConfigurationViewsToVector(m_systemId, m_viewConfigType);

        // Create and cache view buffer for xrLocateViews later.
        m_views.resize(m_configViews.size(), {XR_TYPE_VIEW});

        // Create the swapchain and get the images.
        if (!m_configViews.empty()) {
            // Select a swapchain format.
            std::vector<int64_t> swapchainFormats = m_session.enumerateSwapchainFormatsToVector();
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
                swapchain.handle = get(m_session.createSwapchain(swapchainCreateInfo));

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
        xr::Result res = m_instance.pollEvent(m_eventDataBuffer);
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

    void HandleSessionStateChangedEvent(const xr::EventDataSessionStateChanged& stateChangedEvent, bool* exitRenderLoop,
                                        bool* requestRestart) {
        const xr::SessionState oldState = m_sessionState;
        m_sessionState = stateChangedEvent.state;

        Log::Write(Log::Level::Info,
                   Fmt("XrEventDataSessionStateChanged: state %s->%s session=%lld time=%lld", to_string(oldState).c_str(),
                       to_string(m_sessionState).c_str(), get(stateChangedEvent.session), get(stateChangedEvent.time)));

        if ((stateChangedEvent.session != nullptr) && (stateChangedEvent.session != m_session)) {
            Log::Write(Log::Level::Error, "XrEventDataSessionStateChanged for unknown session");
            return;
        }

        switch (m_sessionState) {
            case xr::SessionState::Ready: {
                CHECK(m_session);
                xr::SessionBeginInfo sessionBeginInfo;
                sessionBeginInfo.primaryViewConfigurationType = m_viewConfigType;
                m_session.beginSession(sessionBeginInfo);
                m_sessionRunning = true;
                break;
            }
            case xr::SessionState::Stopping: {
                CHECK(m_session);
                m_sessionRunning = false;
                m_session.endSession();
                break;
            }
            case xr::SessionState::Exiting: {
                *exitRenderLoop = true;
                // Do not attempt to restart because user closed this session.
                *requestRestart = false;
                break;
            }
            case xr::SessionState::LossPending: {
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

    bool IsSessionFocused() const override { return m_sessionState == xr::SessionState::Focused; }

    void PollActions() override {
        m_input.handActive = {XR_FALSE, XR_FALSE};

        // Sync actions
        const xr::ActiveActionSet activeActionSet{m_input.actionSet, xr::Path::null()};
        m_session.syncActions(xr::ActionsSyncInfo{1, &activeActionSet});

        // Get pose and grab action state and start haptic vibrate when hand is 90% squeezed.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            xr::ActionStateGetInfo getInfo;
            getInfo.action = m_input.grabAction;
            getInfo.subactionPath = m_input.handSubactionPath[hand];

            xr::ActionStateFloat grabValue = m_session.getActionStateFloat(getInfo);
            if (grabValue.isActive == XR_TRUE) {
                // Scale the rendered hand by 1.0f (open) to 0.5f (fully squeezed).
                m_input.handScale[hand] = 1.0f - 0.5f * grabValue.currentState;
                if (grabValue.currentState > 0.9f) {
                    xr::HapticVibration vibration;
                    vibration.amplitude = 0.5;
                    vibration.duration = xr::Duration::minHaptic();
                    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

                    xr::HapticActionInfo hapticActionInfo;
                    hapticActionInfo.action = m_input.vibrateAction;
                    hapticActionInfo.subactionPath = m_input.handSubactionPath[hand];
                    m_session.applyHapticFeedback(hapticActionInfo, get_base(vibration));
                }
            }

            getInfo.action = m_input.poseAction;
            xr::ActionStatePose poseState = m_session.getActionStatePose(getInfo);
            m_input.handActive[hand] = get(poseState.isActive);
        }

        // There were no subaction paths specified for the quit action, because we don't care which hand did it.
        xr::ActionStateBoolean quitValue = m_session.getActionStateBoolean({m_input.quitAction, xr::Path::null()});
        if ((quitValue.isActive == XR_TRUE) && (quitValue.changedSinceLastSync == XR_TRUE) && (quitValue.currentState == XR_TRUE)) {
            m_session.requestExitSession();
        }
    }

    void RenderFrame() override {
        CHECK(m_session);

        xr::FrameState frameState = m_session.waitFrame({});

        m_session.beginFrame({});

        std::vector<const xr::CompositionLayerBaseHeader*> layers;
        xr::CompositionLayerProjection layer;
        std::vector<xr::CompositionLayerProjectionView> projectionLayerViews;
        if (frameState.shouldRender == XR_TRUE) {
            if (RenderLayer(frameState.predictedDisplayTime, projectionLayerViews, layer)) {
                layers.push_back(&layer);
            }
        }

        xr::FrameEndInfo frameEndInfo;
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_environmentBlendMode;
        frameEndInfo.layerCount = (uint32_t)layers.size();
        frameEndInfo.layers = layers.data();
        CHECK_XRCMD(xrEndFrame(get(m_session), get(frameEndInfo)));
    }

    bool RenderLayer(xr::Time predictedDisplayTime, std::vector<xr::CompositionLayerProjectionView>& projectionLayerViews,
                     xr::CompositionLayerProjection& layer) {
        xr::ViewState viewState;
        uint32_t viewCapacityInput = (uint32_t)m_views.size();
        uint32_t viewCountOutput;

        xr::ViewLocateInfo viewLocateInfo;
        viewLocateInfo.viewConfigurationType = m_viewConfigType;
        viewLocateInfo.displayTime = predictedDisplayTime;
        viewLocateInfo.space = m_appSpace;
        xr::Result res = m_session.locateViews({m_viewConfigType, predictedDisplayTime, m_appSpace}, put(viewState),
                                               viewCapacityInput, &viewCountOutput, m_views.data());
        CHECK_XRRESULT(get(res), "xrLocateViews");

        if ((viewState.viewStateFlags & xr::ViewStateFlagBits::PositionValid) == 0 ||
            (viewState.viewStateFlags & xr::ViewStateFlagBits::OrientationValid) == 0) {
            return false;  // There is no valid tracking poses for the views.
        }

        CHECK(viewCountOutput == viewCapacityInput);
        CHECK(viewCountOutput == m_configViews.size());
        CHECK(viewCountOutput == m_swapchains.size());

        // For each locatable space that we want to visualize, render a 25cm cube.
        std::vector<Cube> cubes;

        for (xr::Space visualizedSpace : m_visualizedSpaces) {
            xr::SpaceLocation spaceLocation;
            res = visualizedSpace.locateSpace(m_appSpace, xr::Time(predictedDisplayTime), spaceLocation);
            CHECK_XRRESULT(get(res), "xrLocateSpace");
            if (unqualifiedSuccess(res)) {
                if ((spaceLocation.locationFlags & xr::SpaceLocationFlagBits::PositionValid) != 0 &&
                    (spaceLocation.locationFlags & xr::SpaceLocationFlagBits::OrientationValid) != 0) {
                    cubes.push_back(Cube{spaceLocation.pose, {0.25f, 0.25f, 0.25f}});
                }
            } else {
                Log::Write(Log::Level::Verbose, Fmt("Unable to locate a visualized reference space in app space: %d", res));
            }
        }

        // Render a 10cm cube scaled by grabAction for each hand. Note renderHand will only be
        // true when the application has focus.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            xr::SpaceLocation spaceLocation;
            res = m_input.handSpace[hand].locateSpace(m_appSpace, predictedDisplayTime, spaceLocation);
            CHECK_XRRESULT(get(res), "xrLocateSpace");
            if (unqualifiedSuccess(res)) {
                if ((spaceLocation.locationFlags & xr::SpaceLocationFlagBits::PositionValid) &&
                    (spaceLocation.locationFlags & xr::SpaceLocationFlagBits::OrientationValid) != 0) {
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

        projectionLayerViews.resize(viewCountOutput);
        // Render view to the appropriate part of the swapchain image.
        for (uint32_t i = 0; i < viewCountOutput; i++) {
            // Each view has a separate swapchain which is acquired, rendered to, and released.
            const Swapchain viewSwapchain = m_swapchains[i];
            xr::Swapchain swapchainHandle{viewSwapchain.handle};

            uint32_t swapchainImageIndex = swapchainHandle.acquireSwapchainImage({});

            CHECK_XRCMD(get(swapchainHandle.waitSwapchainImage({xr::Duration::infinite()})));

            projectionLayerViews[i].pose = m_views[i].pose;
            projectionLayerViews[i].fov = m_views[i].fov;
            projectionLayerViews[i].subImage.swapchain = viewSwapchain.handle;
            projectionLayerViews[i].subImage.imageRect.offset = xr::Offset2Di{0, 0};
            projectionLayerViews[i].subImage.imageRect.extent = xr::Extent2Di{viewSwapchain.width, viewSwapchain.height};

            const XrSwapchainImageBaseHeader* const swapchainImage = m_swapchainImages[viewSwapchain.handle][swapchainImageIndex];
            m_graphicsPlugin->RenderView(projectionLayerViews[i], swapchainImage, m_colorSwapchainFormat, cubes);

            swapchainHandle.releaseSwapchainImage({});
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
    xr::Instance m_instance;
    xr::Session m_session;
    xr::Space m_appSpace;
    xr::FormFactor m_formFactor{xr::FormFactor::HeadMountedDisplay};
    xr::ViewConfigurationType m_viewConfigType{xr::ViewConfigurationType::PrimaryStereo};
    xr::EnvironmentBlendMode m_environmentBlendMode{xr::EnvironmentBlendMode::Opaque};
    xr::SystemId m_systemId;

    std::vector<xr::ViewConfigurationView> m_configViews;
    std::vector<Swapchain> m_swapchains;
    std::map<XrSwapchain, std::vector<XrSwapchainImageBaseHeader*>> m_swapchainImages;
    std::vector<XrView> m_views;
    int64_t m_colorSwapchainFormat{-1};

    std::vector<xr::Space> m_visualizedSpaces;

    // Application's current lifecycle state according to the runtime
    xr::SessionState m_sessionState;
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
