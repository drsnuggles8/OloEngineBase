#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Characters.cpp — character controllers, physics joints, and the vehicle / boat / sail / aircraft / ragdoll / cloth family.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterCharacterTypes(sol::state& lua)
    {
        // --- CharacterController3DComponent ---
        lua.new_usertype<CharacterController3DComponent>("CharacterController3DComponent",
                                                         "slopeLimitDeg", sol::property([](const CharacterController3DComponent& c)
                                                                                        { return c.m_SlopeLimitDeg; }, [](CharacterController3DComponent& c, f32 v)
                                                                                        { if (std::isfinite(v) && v >= 0.0f && v <= 90.0f) c.m_SlopeLimitDeg = v; }),
                                                         "stepOffset", sol::property([](const CharacterController3DComponent& c)
                                                                                     { return c.m_StepOffset; }, [](CharacterController3DComponent& c, f32 v)
                                                                                     { if (std::isfinite(v) && v >= 0.0f) c.m_StepOffset = v; }),
                                                         "jumpPower", sol::property([](const CharacterController3DComponent& c)
                                                                                    { return c.m_JumpPower; }, [](CharacterController3DComponent& c, f32 v)
                                                                                    { if (std::isfinite(v) && v >= 0.0f) c.m_JumpPower = v; }),
                                                         "layerID", sol::property([](const CharacterController3DComponent& c)
                                                                                  { return c.m_LayerID; }, [](CharacterController3DComponent& c, int v)
                                                                                  { if (v >= 0) c.m_LayerID = static_cast<u32>(v); }),
                                                         "disableGravity", &CharacterController3DComponent::m_DisableGravity,
                                                         "controlMovementInAir", &CharacterController3DComponent::m_ControlMovementInAir,
                                                         "controlRotationInAir", &CharacterController3DComponent::m_ControlRotationInAir);

        // --- PhysicsJoint3DComponent ---
        lua.new_usertype<PhysicsJoint3DComponent>("PhysicsJoint3DComponent",
                                                  "jointType", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                             { return std::to_underlying(j.m_Type); }, [](PhysicsJoint3DComponent& j, int v)
                                                                             { if (v >= 0 && v <= static_cast<int>(JointType3D::Path)) j.m_Type = static_cast<JointType3D>(v); }),
                                                  "connectedEntity", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                   { return static_cast<u64>(j.m_ConnectedEntity); }, [](PhysicsJoint3DComponent& j, u64 v)
                                                                                   { j.m_ConnectedEntity = UUID(v); }),
                                                  "localAnchorA", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                { return j.m_LocalAnchorA; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                { if (IsFiniteVec3(v)) j.m_LocalAnchorA = v; }),
                                                  "localAnchorB", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                { return j.m_LocalAnchorB; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                { if (IsFiniteVec3(v)) j.m_LocalAnchorB = v; }),
                                                  "axis", sol::property([](const PhysicsJoint3DComponent& j)
                                                                        { return j.m_Axis; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                        { if (IsFiniteVec3(v)) j.m_Axis = v; }),
                                                  "minDistance", sol::property([](const PhysicsJoint3DComponent& j)
                                                                               { return j.m_MinDistance; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                               { if (std::isfinite(v)) j.m_MinDistance = v; }),
                                                  "maxDistance", sol::property([](const PhysicsJoint3DComponent& j)
                                                                               { return j.m_MaxDistance; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                               { if (std::isfinite(v)) j.m_MaxDistance = v; }),
                                                  "hingeMinAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_HingeMinAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                    { if (std::isfinite(v)) j.m_HingeMinAngleDeg = v; }),
                                                  "hingeMaxAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_HingeMaxAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                    { if (std::isfinite(v)) j.m_HingeMaxAngleDeg = v; }),
                                                  "sliderMinLimit", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                  { return j.m_SliderMinLimit; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                  { if (std::isfinite(v)) j.m_SliderMinLimit = v; }),
                                                  "sliderMaxLimit", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                  { return j.m_SliderMaxLimit; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                  { if (std::isfinite(v)) j.m_SliderMaxLimit = v; }),
                                                  "coneHalfAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_ConeHalfAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                    { if (std::isfinite(v) && v >= 0.0f) j.m_ConeHalfAngleDeg = v; }),
                                                  // Break thresholds (N / N·m). 0 means unbreakable on that axis.
                                                  // Clamp to the same [0, 1e9] range the serializers enforce so a
                                                  // script-set value round-trips through save/load unchanged.
                                                  "breakForce", sol::property([](const PhysicsJoint3DComponent& j)
                                                                              { return j.m_BreakForce; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                              { if (std::isfinite(v)) j.m_BreakForce = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "breakTorque", sol::property([](const PhysicsJoint3DComponent& j)
                                                                               { return j.m_BreakTorque; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                               { if (std::isfinite(v)) j.m_BreakTorque = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  // Hinge motor + friction. Mode is exposed as an int
                                                  // (0=Off, 1=Velocity, 2=Position); target velocity is deg/s
                                                  // and target angle is deg. Max torque / friction are
                                                  // magnitudes clamped to [0, 1e9] (0 = no authority/friction).
                                                  "hingeMotorMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                  { return std::to_underlying(j.m_HingeMotorMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                  { if (v >= 0 && v <= static_cast<int>(JointMotorMode::Position)) j.m_HingeMotorMode = static_cast<JointMotorMode>(v); }),
                                                  "hingeMotorTargetVelocityDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                               { return j.m_HingeMotorTargetVelocityDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                               { if (std::isfinite(v)) j.m_HingeMotorTargetVelocityDeg = v; }),
                                                  "hingeMotorTargetAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                            { return j.m_HingeMotorTargetAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                            { if (std::isfinite(v)) j.m_HingeMotorTargetAngleDeg = v; }),
                                                  "hingeMaxMotorTorque", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                       { return j.m_HingeMaxMotorTorque; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                       { if (std::isfinite(v)) j.m_HingeMaxMotorTorque = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "hingeMaxFrictionTorque", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                          { return j.m_HingeMaxFrictionTorque; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                          { if (std::isfinite(v)) j.m_HingeMaxFrictionTorque = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  // Springy (soft) hinge limits: frequency in Hz (0 = hard limits)
                                                  // and damping ratio (0 = undamped, 1 = critical).
                                                  "hingeLimitSpringFrequency", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                             { return j.m_HingeLimitSpringFrequency; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                             { if (std::isfinite(v)) j.m_HingeLimitSpringFrequency = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "hingeLimitSpringDamping", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                           { return j.m_HingeLimitSpringDamping; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                           { if (std::isfinite(v)) j.m_HingeLimitSpringDamping = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  // Slider motor + friction. Target velocity is m/s, target
                                                  // position is m; max force / friction are magnitudes in N.
                                                  "sliderMotorMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                   { return std::to_underlying(j.m_SliderMotorMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                   { if (v >= 0 && v <= static_cast<int>(JointMotorMode::Position)) j.m_SliderMotorMode = static_cast<JointMotorMode>(v); }),
                                                  "sliderMotorTargetVelocity", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                             { return j.m_SliderMotorTargetVelocity; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                             { if (std::isfinite(v)) j.m_SliderMotorTargetVelocity = v; }),
                                                  "sliderMotorTargetPosition", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                             { return j.m_SliderMotorTargetPosition; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                             { if (std::isfinite(v)) j.m_SliderMotorTargetPosition = v; }),
                                                  "sliderMaxMotorForce", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                       { return j.m_SliderMaxMotorForce; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                       { if (std::isfinite(v)) j.m_SliderMaxMotorForce = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "sliderMaxFrictionForce", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                          { return j.m_SliderMaxFrictionForce; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                          { if (std::isfinite(v)) j.m_SliderMaxFrictionForce = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  // Springy (soft) slider limits — same shape as the hinge.
                                                  "sliderLimitSpringFrequency", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                              { return j.m_SliderLimitSpringFrequency; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                              { if (std::isfinite(v)) j.m_SliderLimitSpringFrequency = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "sliderLimitSpringDamping", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                            { return j.m_SliderLimitSpringDamping; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                            { if (std::isfinite(v)) j.m_SliderLimitSpringDamping = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  // SwingTwist joint: swing cone half-angles (deg, clamped to [0,180])
                                                  // about the derived plane normal / in-plane direction, and the twist
                                                  // range about m_Axis (deg, clamped to [-180,180]).
                                                  "swingNormalHalfAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                           { return j.m_SwingNormalHalfAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                           { if (std::isfinite(v)) j.m_SwingNormalHalfAngleDeg = std::clamp(v, 0.0f, 180.0f); }),
                                                  "swingPlaneHalfAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                          { return j.m_SwingPlaneHalfAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                          { if (std::isfinite(v)) j.m_SwingPlaneHalfAngleDeg = std::clamp(v, 0.0f, 180.0f); }),
                                                  "twistMinAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_TwistMinAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                    { if (std::isfinite(v)) j.m_TwistMinAngleDeg = std::clamp(v, -180.0f, 180.0f); }),
                                                  "twistMaxAngleDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_TwistMaxAngleDeg; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                    { if (std::isfinite(v)) j.m_TwistMaxAngleDeg = std::clamp(v, -180.0f, 180.0f); }),
                                                  // SixDOF per-axis modes, exposed as ints (0=Locked, 1=Limited, 2=Free).
                                                  "sixDOFTransXMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                    { return std::to_underlying(j.m_SixDOFTransXMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                    { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFTransXMode = static_cast<JointAxisMode>(v); }),
                                                  "sixDOFTransYMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                    { return std::to_underlying(j.m_SixDOFTransYMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                    { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFTransYMode = static_cast<JointAxisMode>(v); }),
                                                  "sixDOFTransZMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                    { return std::to_underlying(j.m_SixDOFTransZMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                    { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFTransZMode = static_cast<JointAxisMode>(v); }),
                                                  "sixDOFRotXMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                  { return std::to_underlying(j.m_SixDOFRotXMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                  { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFRotXMode = static_cast<JointAxisMode>(v); }),
                                                  "sixDOFRotYMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                  { return std::to_underlying(j.m_SixDOFRotYMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                  { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFRotYMode = static_cast<JointAxisMode>(v); }),
                                                  "sixDOFRotZMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                  { return std::to_underlying(j.m_SixDOFRotZMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                  { if (v >= 0 && v <= static_cast<int>(JointAxisMode::Free)) j.m_SixDOFRotZMode = static_cast<JointAxisMode>(v); }),
                                                  // SixDOF limits used by axes in Limited mode (translation m, rotation deg).
                                                  "sixDOFTranslationMin", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                        { return j.m_SixDOFTranslationMin; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                        { if (IsFiniteVec3(v)) j.m_SixDOFTranslationMin = v; }),
                                                  "sixDOFTranslationMax", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                        { return j.m_SixDOFTranslationMax; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                        { if (IsFiniteVec3(v)) j.m_SixDOFTranslationMax = v; }),
                                                  "sixDOFRotationMinDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                        { return j.m_SixDOFRotationMinDeg; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                        { if (IsFiniteVec3(v)) j.m_SixDOFRotationMinDeg = v; }),
                                                  "sixDOFRotationMaxDeg", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                        { return j.m_SixDOFRotationMaxDeg; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                        { if (IsFiniteVec3(v)) j.m_SixDOFRotationMaxDeg = v; }),
                                                  "collideConnected", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                    { return j.m_CollideConnected; }, [](PhysicsJoint3DComponent& j, bool v)
                                                                                    { j.m_CollideConnected = v; }),
                                                  // Pulley (world-space fixed points + ratio/length) — issue #308.
                                                  "pulleyFixedPointA", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                     { return j.m_PulleyFixedPointA; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                     { if (IsFiniteVec3(v)) j.m_PulleyFixedPointA = v; }),
                                                  "pulleyFixedPointB", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                     { return j.m_PulleyFixedPointB; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                     { if (IsFiniteVec3(v)) j.m_PulleyFixedPointB = v; }),
                                                  "pulleyRatio", sol::property([](const PhysicsJoint3DComponent& j)
                                                                               { return j.m_PulleyRatio; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                               { if (std::isfinite(v)) j.m_PulleyRatio = std::clamp(v, 0.0f, 1.0e9f); }), // length multiplier → non-negative
                                                  "pulleyMinLength", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                   { return j.m_PulleyMinLength; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                   { if (std::isfinite(v)) j.m_PulleyMinLength = std::clamp(v, -1.0f, 1.0e9f); }), // -1 = auto-length sentinel
                                                  "pulleyMaxLength", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                   { return j.m_PulleyMaxLength; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                   { if (std::isfinite(v)) j.m_PulleyMaxLength = std::clamp(v, -1.0f, 1.0e9f); }),
                                                  // Gear / RackAndPinion (connected-body axis + ratio) — issue #308.
                                                  "connectedAxis", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                 { return j.m_ConnectedAxis; }, [](PhysicsJoint3DComponent& j, const glm::vec3& v)
                                                                                 { if (IsFiniteVec3(v)) j.m_ConnectedAxis = v; }),
                                                  "gearRatio", sol::property([](const PhysicsJoint3DComponent& j)
                                                                             { return j.m_GearRatio; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                             { if (std::isfinite(v)) j.m_GearRatio = v; }),
                                                  // Path joint (issue #308). pathPoints is a 1-indexed Lua table of
                                                  // vec3 control points (path-local space); the setter drops any
                                                  // non-finite point. The motor drives the body ALONG the path.
                                                  "pathPoints", sol::property([](const PhysicsJoint3DComponent& j, sol::this_state s) -> sol::table
                                                                              {
                                                                                  sol::state_view lua_state(s);
                                                                                  sol::table t = lua_state.create_table(static_cast<int>(j.m_PathPoints.size()), 0);
                                                                                  for (sizet i = 0; i < j.m_PathPoints.size(); ++i)
                                                                                      t[i + 1] = j.m_PathPoints[i];
                                                                                  return t; }, [](PhysicsJoint3DComponent& j, const sol::table& t)
                                                                              {
                                                                                  std::vector<glm::vec3> pts;
                                                                                  pts.reserve(t.size());
                                                                                  for (sizet i = 1; i <= t.size(); ++i)
                                                                                  {
                                                                                      if (sol::optional<glm::vec3> v = t[i]; v && IsFiniteVec3(*v))
                                                                                          pts.push_back(*v);
                                                                                  }
                                                                                  j.m_PathPoints = std::move(pts); }),
                                                  "pathIsLooping", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                 { return j.m_PathIsLooping; }, [](PhysicsJoint3DComponent& j, bool v)
                                                                                 { j.m_PathIsLooping = v; }),
                                                  // Rotation mode (0=Free .. 5=FullyConstrained) and motor mode
                                                  // (0=Off, 1=Velocity, 2=Position) are ints, guarded to range.
                                                  "pathRotationMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                    { return std::to_underlying(j.m_PathRotationMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                    { if (v >= 0 && v <= static_cast<int>(JointPathRotationMode::FullyConstrained)) j.m_PathRotationMode = static_cast<JointPathRotationMode>(v); }),
                                                  "pathMotorMode", sol::property([](const PhysicsJoint3DComponent& j) -> int
                                                                                 { return std::to_underlying(j.m_PathMotorMode); }, [](PhysicsJoint3DComponent& j, int v)
                                                                                 { if (v >= 0 && v <= static_cast<int>(JointMotorMode::Position)) j.m_PathMotorMode = static_cast<JointMotorMode>(v); }),
                                                  // Target velocity is m/s along the path (signed); target fraction is
                                                  // a non-negative path coordinate; max force/friction are magnitudes.
                                                  "pathMotorTargetVelocity", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                           { return j.m_PathMotorTargetVelocity; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                           { if (std::isfinite(v)) j.m_PathMotorTargetVelocity = v; }),
                                                  "pathMotorTargetFraction", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                           { return j.m_PathMotorTargetFraction; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                           { if (std::isfinite(v)) j.m_PathMotorTargetFraction = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "pathMaxMotorForce", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                     { return j.m_PathMaxMotorForce; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                     { if (std::isfinite(v)) j.m_PathMaxMotorForce = std::clamp(v, 0.0f, 1.0e9f); }),
                                                  "pathMaxFrictionForce", sol::property([](const PhysicsJoint3DComponent& j)
                                                                                        { return j.m_PathMaxFrictionForce; }, [](PhysicsJoint3DComponent& j, f32 v)
                                                                                        { if (std::isfinite(v)) j.m_PathMaxFrictionForce = std::clamp(v, 0.0f, 1.0e9f); }));

        // --- VehicleComponent ---
        // Authored geometry / drivetrain setters validate finiteness + positivity
        // (a degenerate value would NaN the Jolt vehicle); the live driver inputs
        // (throttle/steer/brake) are what gameplay scripts drive each frame, so
        // they clamp to their valid ranges. m_RuntimeVehicleToken is not exposed.
        lua.new_usertype<VehicleComponent>("VehicleComponent",
                                           "halfTrackWidth", sol::property([](const VehicleComponent& v)
                                                                           { return v.m_HalfTrackWidth; }, [](VehicleComponent& v, f32 x)
                                                                           { if (std::isfinite(x) && x > 0.0f) v.m_HalfTrackWidth = x; }),
                                           "frontAxleOffset", sol::property([](const VehicleComponent& v)
                                                                            { return v.m_FrontAxleOffset; }, [](VehicleComponent& v, f32 x)
                                                                            { if (std::isfinite(x) && x > 0.0f) v.m_FrontAxleOffset = x; }),
                                           "rearAxleOffset", sol::property([](const VehicleComponent& v)
                                                                           { return v.m_RearAxleOffset; }, [](VehicleComponent& v, f32 x)
                                                                           { if (std::isfinite(x) && x > 0.0f) v.m_RearAxleOffset = x; }),
                                           "wheelAttachmentHeight", sol::property([](const VehicleComponent& v)
                                                                                  { return v.m_WheelAttachmentHeight; }, [](VehicleComponent& v, f32 x)
                                                                                  { if (std::isfinite(x)) v.m_WheelAttachmentHeight = x; }),
                                           "wheelRadius", sol::property([](const VehicleComponent& v)
                                                                        { return v.m_WheelRadius; }, [](VehicleComponent& v, f32 x)
                                                                        { if (std::isfinite(x) && x > 0.0f) v.m_WheelRadius = x; }),
                                           "wheelWidth", sol::property([](const VehicleComponent& v)
                                                                       { return v.m_WheelWidth; }, [](VehicleComponent& v, f32 x)
                                                                       { if (std::isfinite(x) && x > 0.0f) v.m_WheelWidth = x; }),
                                           "suspensionMinLength", sol::property([](const VehicleComponent& v)
                                                                                { return v.m_SuspensionMinLength; }, [](VehicleComponent& v, f32 x)
                                                                                { if (std::isfinite(x) && x >= 0.0f) v.m_SuspensionMinLength = x; }),
                                           "suspensionMaxLength", sol::property([](const VehicleComponent& v)
                                                                                { return v.m_SuspensionMaxLength; }, [](VehicleComponent& v, f32 x)
                                                                                { if (std::isfinite(x) && x >= 0.0f) v.m_SuspensionMaxLength = x; }),
                                           "suspensionFrequency", sol::property([](const VehicleComponent& v)
                                                                                { return v.m_SuspensionFrequency; }, [](VehicleComponent& v, f32 x)
                                                                                { if (std::isfinite(x) && x > 0.0f) v.m_SuspensionFrequency = x; }),
                                           "suspensionDamping", sol::property([](const VehicleComponent& v)
                                                                              { return v.m_SuspensionDamping; }, [](VehicleComponent& v, f32 x)
                                                                              { if (std::isfinite(x)) v.m_SuspensionDamping = std::clamp(x, 0.0f, 1.0f); }),
                                           "maxEngineTorque", sol::property([](const VehicleComponent& v)
                                                                            { return v.m_MaxEngineTorque; }, [](VehicleComponent& v, f32 x)
                                                                            { if (std::isfinite(x) && x >= 0.0f) v.m_MaxEngineTorque = x; }),
                                           "maxSteerAngleDeg", sol::property([](const VehicleComponent& v)
                                                                             { return v.m_MaxSteerAngleDeg; }, [](VehicleComponent& v, f32 x)
                                                                             { if (std::isfinite(x)) v.m_MaxSteerAngleDeg = std::clamp(x, 0.0f, 180.0f); }),
                                           "maxBrakeTorque", sol::property([](const VehicleComponent& v)
                                                                           { return v.m_MaxBrakeTorque; }, [](VehicleComponent& v, f32 x)
                                                                           { if (std::isfinite(x) && x >= 0.0f) v.m_MaxBrakeTorque = x; }),
                                           // Differentials (issue #438): driveMode is an int matching
                                           // VehicleDriveMode (0 = RWD, 1 = FWD, 2 = AWD); an out-of-range
                                           // write is ignored rather than producing an invalid enum.
                                           "driveMode", sol::property([](const VehicleComponent& v)
                                                                      { return static_cast<int>(v.m_DriveMode); }, [](VehicleComponent& v, int x)
                                                                      { if (x >= 0 && x <= 2) v.m_DriveMode = static_cast<VehicleDriveMode>(x); }),
                                           "frontTorqueSplit", sol::property([](const VehicleComponent& v)
                                                                             { return v.m_FrontTorqueSplit; }, [](VehicleComponent& v, f32 x)
                                                                             { if (std::isfinite(x)) v.m_FrontTorqueSplit = std::clamp(x, 0.0f, 1.0f); }),
                                           "leftRightSplit", sol::property([](const VehicleComponent& v)
                                                                           { return v.m_LeftRightSplit; }, [](VehicleComponent& v, f32 x)
                                                                           { if (std::isfinite(x)) v.m_LeftRightSplit = std::clamp(x, 0.0f, 1.0f); }),
                                           "limitedSlipRatio", sol::property([](const VehicleComponent& v)
                                                                             { return v.m_LimitedSlipRatio; }, [](VehicleComponent& v, f32 x)
                                                                             { if (std::isfinite(x)) v.m_LimitedSlipRatio = std::clamp(x, 1.001f, 1.0e6f); }),
                                           "centerLimitedSlipRatio", sol::property([](const VehicleComponent& v)
                                                                                   { return v.m_CenterLimitedSlipRatio; }, [](VehicleComponent& v, f32 x)
                                                                                   { if (std::isfinite(x)) v.m_CenterLimitedSlipRatio = std::clamp(x, 1.001f, 1.0e6f); }),
                                           "differentialRatio", sol::property([](const VehicleComponent& v)
                                                                              { return v.m_DifferentialRatio; }, [](VehicleComponent& v, f32 x)
                                                                              { if (std::isfinite(x) && x > 0.0f) v.m_DifferentialRatio = x; }),
                                           "throttleInput", sol::property([](const VehicleComponent& v)
                                                                          { return v.m_ThrottleInput; }, [](VehicleComponent& v, f32 x)
                                                                          { if (std::isfinite(x)) v.m_ThrottleInput = std::clamp(x, -1.0f, 1.0f); }),
                                           "steerInput", sol::property([](const VehicleComponent& v)
                                                                       { return v.m_SteerInput; }, [](VehicleComponent& v, f32 x)
                                                                       { if (std::isfinite(x)) v.m_SteerInput = std::clamp(x, -1.0f, 1.0f); }),
                                           "brakeInput", sol::property([](const VehicleComponent& v)
                                                                       { return v.m_BrakeInput; }, [](VehicleComponent& v, f32 x)
                                                                       { if (std::isfinite(x)) v.m_BrakeInput = std::clamp(x, 0.0f, 1.0f); }));

        // --- BoatComponent (issue #438) ---
        // The live driver inputs (throttle/steer) are what gameplay scripts drive
        // each frame and clamp to their valid ranges; the authored hull tuning
        // validates finiteness + sign so a scripted garbage value can't NaN the
        // force model. BoatSystem re-sanitizes as a last line of defence.
        lua.new_usertype<BoatComponent>("BoatComponent",
                                        "enabled", sol::property([](const BoatComponent& b)
                                                                 { return b.m_Enabled; }, [](BoatComponent& b, bool x)
                                                                 { b.m_Enabled = x; }),
                                        "maxThrust", sol::property([](const BoatComponent& b)
                                                                   { return b.m_MaxThrust; }, [](BoatComponent& b, f32 x)
                                                                   { if (std::isfinite(x) && x >= 0.0f) b.m_MaxThrust = x; }),
                                        "thrustOffsetZ", sol::property([](const BoatComponent& b)
                                                                       { return b.m_ThrustOffsetZ; }, [](BoatComponent& b, f32 x)
                                                                       { if (std::isfinite(x)) b.m_ThrustOffsetZ = std::clamp(x, -1000.0f, 1000.0f); }),
                                        "thrustOffsetY", sol::property([](const BoatComponent& b)
                                                                       { return b.m_ThrustOffsetY; }, [](BoatComponent& b, f32 x)
                                                                       { if (std::isfinite(x)) b.m_ThrustOffsetY = std::clamp(x, -1000.0f, 1000.0f); }),
                                        "maxRudderTorque", sol::property([](const BoatComponent& b)
                                                                         { return b.m_MaxRudderTorque; }, [](BoatComponent& b, f32 x)
                                                                         { if (std::isfinite(x) && x >= 0.0f) b.m_MaxRudderTorque = x; }),
                                        "rudderAuthoritySpeed", sol::property([](const BoatComponent& b)
                                                                              { return b.m_RudderAuthoritySpeed; }, [](BoatComponent& b, f32 x)
                                                                              { if (std::isfinite(x) && x > 0.0f) b.m_RudderAuthoritySpeed = x; }),
                                        "lateralDrag", sol::property([](const BoatComponent& b)
                                                                     { return b.m_LateralDrag; }, [](BoatComponent& b, f32 x)
                                                                     { if (std::isfinite(x) && x >= 0.0f) b.m_LateralDrag = x; }),
                                        "forwardDrag", sol::property([](const BoatComponent& b)
                                                                     { return b.m_ForwardDrag; }, [](BoatComponent& b, f32 x)
                                                                     { if (std::isfinite(x) && x >= 0.0f) b.m_ForwardDrag = x; }),
                                        "yawDrag", sol::property([](const BoatComponent& b)
                                                                 { return b.m_YawDrag; }, [](BoatComponent& b, f32 x)
                                                                 { if (std::isfinite(x) && x >= 0.0f) b.m_YawDrag = x; }),
                                        "immersionDepth", sol::property([](const BoatComponent& b)
                                                                        { return b.m_ImmersionDepth; }, [](BoatComponent& b, f32 x)
                                                                        { if (std::isfinite(x) && x > 0.0f) b.m_ImmersionDepth = x; }),
                                        "throttleInput", sol::property([](const BoatComponent& b)
                                                                       { return b.m_ThrottleInput; }, [](BoatComponent& b, f32 x)
                                                                       { if (std::isfinite(x)) b.m_ThrottleInput = std::clamp(x, -1.0f, 1.0f); }),
                                        "steerInput", sol::property([](const BoatComponent& b)
                                                                    { return b.m_SteerInput; }, [](BoatComponent& b, f32 x)
                                                                    { if (std::isfinite(x)) b.m_SteerInput = std::clamp(x, -1.0f, 1.0f); }));

        // --- SailComponent (issue #899) ---
        // The rig tuning validates finiteness + range; the driver inputs clamp.
        // The RUNTIME READOUTS are exposed READ-ONLY on purpose: SailSystem is
        // their only writer, and a script that could assign m_YardAngle would be
        // able to desync the drawn sail from the force actually being applied —
        // which is precisely the "sail is decoration" bug this component exists
        // to end. Read yardAngle and copy it onto the sail mesh entity's Y
        // rotation; do not invent one.
        lua.new_usertype<SailComponent>("SailComponent",
                                        "enabled", sol::property([](const SailComponent& s)
                                                                 { return s.m_Enabled; }, [](SailComponent& s, bool x)
                                                                 { s.m_Enabled = x; }),
                                        "sailArea", sol::property([](const SailComponent& s)
                                                                  { return s.m_SailArea; }, [](SailComponent& s, f32 x)
                                                                  { if (std::isfinite(x) && x >= 0.0f) s.m_SailArea = x; }),
                                        "airDensity", sol::property([](const SailComponent& s)
                                                                    { return s.m_AirDensity; }, [](SailComponent& s, f32 x)
                                                                    { if (std::isfinite(x) && x >= 0.0f) s.m_AirDensity = x; }),
                                        "maxNormalCoefficient", sol::property([](const SailComponent& s)
                                                                              { return s.m_MaxNormalCoefficient; }, [](SailComponent& s, f32 x)
                                                                              { if (std::isfinite(x) && x >= 0.0f) s.m_MaxNormalCoefficient = x; }),
                                        "maxYardAngleDeg", sol::property([](const SailComponent& s)
                                                                         { return s.m_MaxYardAngleDeg; }, [](SailComponent& s, f32 x)
                                                                         { if (std::isfinite(x)) s.m_MaxYardAngleDeg = std::clamp(x, 0.0f, 90.0f); }),
                                        "trimRateDeg", sol::property([](const SailComponent& s)
                                                                     { return s.m_TrimRateDeg; }, [](SailComponent& s, f32 x)
                                                                     { if (std::isfinite(x) && x >= 0.0f) s.m_TrimRateDeg = x; }),
                                        "centreOfEffortY", sol::property([](const SailComponent& s)
                                                                         { return s.m_CentreOfEffortY; }, [](SailComponent& s, f32 x)
                                                                         { if (std::isfinite(x)) s.m_CentreOfEffortY = std::clamp(x, -1000.0f, 1000.0f); }),
                                        "centreOfEffortZ", sol::property([](const SailComponent& s)
                                                                         { return s.m_CentreOfEffortZ; }, [](SailComponent& s, f32 x)
                                                                         { if (std::isfinite(x)) s.m_CentreOfEffortZ = std::clamp(x, -1000.0f, 1000.0f); }),
                                        "autoTrim", sol::property([](const SailComponent& s)
                                                                  { return s.m_AutoTrim; }, [](SailComponent& s, bool x)
                                                                  { s.m_AutoTrim = x; }),
                                        "trimInput", sol::property([](const SailComponent& s)
                                                                   { return s.m_TrimInput; }, [](SailComponent& s, f32 x)
                                                                   { if (std::isfinite(x)) s.m_TrimInput = std::clamp(x, -1.0f, 1.0f); }),
                                        "sailSetInput", sol::property([](const SailComponent& s)
                                                                      { return s.m_SailSetInput; }, [](SailComponent& s, f32 x)
                                                                      { if (std::isfinite(x)) s.m_SailSetInput = std::clamp(x, 0.0f, 1.0f); }),
                                        // Read-only readouts, written by SailSystem each physics step.
                                        "yardAngle", sol::readonly(&SailComponent::m_YardAngle),
                                        "apparentWindSpeed", sol::readonly(&SailComponent::m_ApparentWindSpeed),
                                        "apparentWindAngle", sol::readonly(&SailComponent::m_ApparentWindAngle),
                                        "driveForce", sol::readonly(&SailComponent::m_DriveForce),
                                        "heelForce", sol::readonly(&SailComponent::m_HeelForce),
                                        "luffing", sol::readonly(&SailComponent::m_Luffing));

        // --- AircraftComponent (issue #438) ---
        // Same split as the boat: pilot inputs clamp, airframe tuning validates.
        lua.new_usertype<AircraftComponent>("AircraftComponent",
                                            "enabled", sol::property([](const AircraftComponent& a)
                                                                     { return a.m_Enabled; }, [](AircraftComponent& a, bool x)
                                                                     { a.m_Enabled = x; }),
                                            "maxThrust", sol::property([](const AircraftComponent& a)
                                                                       { return a.m_MaxThrust; }, [](AircraftComponent& a, f32 x)
                                                                       { if (std::isfinite(x) && x >= 0.0f) a.m_MaxThrust = x; }),
                                            "wingArea", sol::property([](const AircraftComponent& a)
                                                                      { return a.m_WingArea; }, [](AircraftComponent& a, f32 x)
                                                                      { if (std::isfinite(x) && x > 0.0f) a.m_WingArea = x; }),
                                            "airDensity", sol::property([](const AircraftComponent& a)
                                                                        { return a.m_AirDensity; }, [](AircraftComponent& a, f32 x)
                                                                        { if (std::isfinite(x) && x >= 0.0f) a.m_AirDensity = x; }),
                                            "liftSlope", sol::property([](const AircraftComponent& a)
                                                                       { return a.m_LiftSlope; }, [](AircraftComponent& a, f32 x)
                                                                       { if (std::isfinite(x) && x >= 0.0f) a.m_LiftSlope = x; }),
                                            "zeroLiftCoefficient", sol::property([](const AircraftComponent& a)
                                                                                 { return a.m_ZeroLiftCoefficient; }, [](AircraftComponent& a, f32 x)
                                                                                 { if (std::isfinite(x)) a.m_ZeroLiftCoefficient = std::clamp(x, -10.0f, 10.0f); }),
                                            "stallAngleDeg", sol::property([](const AircraftComponent& a)
                                                                           { return a.m_StallAngleDeg; }, [](AircraftComponent& a, f32 x)
                                                                           { if (std::isfinite(x)) a.m_StallAngleDeg = std::clamp(x, 0.1f, 90.0f); }),
                                            "dragCoefficient", sol::property([](const AircraftComponent& a)
                                                                             { return a.m_DragCoefficient; }, [](AircraftComponent& a, f32 x)
                                                                             { if (std::isfinite(x) && x >= 0.0f) a.m_DragCoefficient = x; }),
                                            "inducedDragFactor", sol::property([](const AircraftComponent& a)
                                                                               { return a.m_InducedDragFactor; }, [](AircraftComponent& a, f32 x)
                                                                               { if (std::isfinite(x) && x >= 0.0f) a.m_InducedDragFactor = x; }),
                                            "pitchTorque", sol::property([](const AircraftComponent& a)
                                                                         { return a.m_PitchTorque; }, [](AircraftComponent& a, f32 x)
                                                                         { if (std::isfinite(x) && x >= 0.0f) a.m_PitchTorque = x; }),
                                            "rollTorque", sol::property([](const AircraftComponent& a)
                                                                        { return a.m_RollTorque; }, [](AircraftComponent& a, f32 x)
                                                                        { if (std::isfinite(x) && x >= 0.0f) a.m_RollTorque = x; }),
                                            "yawTorque", sol::property([](const AircraftComponent& a)
                                                                       { return a.m_YawTorque; }, [](AircraftComponent& a, f32 x)
                                                                       { if (std::isfinite(x) && x >= 0.0f) a.m_YawTorque = x; }),
                                            "controlAuthoritySpeed", sol::property([](const AircraftComponent& a)
                                                                                   { return a.m_ControlAuthoritySpeed; }, [](AircraftComponent& a, f32 x)
                                                                                   { if (std::isfinite(x) && x > 0.0f) a.m_ControlAuthoritySpeed = x; }),
                                            "pitchDamping", sol::property([](const AircraftComponent& a)
                                                                          { return a.m_PitchDamping; }, [](AircraftComponent& a, f32 x)
                                                                          { if (std::isfinite(x) && x >= 0.0f) a.m_PitchDamping = x; }),
                                            "rollDamping", sol::property([](const AircraftComponent& a)
                                                                         { return a.m_RollDamping; }, [](AircraftComponent& a, f32 x)
                                                                         { if (std::isfinite(x) && x >= 0.0f) a.m_RollDamping = x; }),
                                            "yawDamping", sol::property([](const AircraftComponent& a)
                                                                        { return a.m_YawDamping; }, [](AircraftComponent& a, f32 x)
                                                                        { if (std::isfinite(x) && x >= 0.0f) a.m_YawDamping = x; }),
                                            "weathervaneStrength", sol::property([](const AircraftComponent& a)
                                                                                 { return a.m_WeathervaneStrength; }, [](AircraftComponent& a, f32 x)
                                                                                 { if (std::isfinite(x) && x >= 0.0f) a.m_WeathervaneStrength = x; }),
                                            // Landing gear (issue #438 follow-up).
                                            "hasLandingGear", sol::property([](const AircraftComponent& a)
                                                                            { return a.m_HasLandingGear; }, [](AircraftComponent& a, bool x)
                                                                            { a.m_HasLandingGear = x; }),
                                            "mainGearOffsetZ", sol::property([](const AircraftComponent& a)
                                                                             { return a.m_MainGearOffsetZ; }, [](AircraftComponent& a, f32 x)
                                                                             { if (std::isfinite(x)) a.m_MainGearOffsetZ = std::clamp(x, -1000.0f, 1000.0f); }),
                                            "mainGearHalfTrack", sol::property([](const AircraftComponent& a)
                                                                               { return a.m_MainGearHalfTrack; }, [](AircraftComponent& a, f32 x)
                                                                               { if (std::isfinite(x) && x > 0.0f) a.m_MainGearHalfTrack = x; }),
                                            "noseGearOffsetZ", sol::property([](const AircraftComponent& a)
                                                                             { return a.m_NoseGearOffsetZ; }, [](AircraftComponent& a, f32 x)
                                                                             { if (std::isfinite(x)) a.m_NoseGearOffsetZ = std::clamp(x, -1000.0f, 1000.0f); }),
                                            "gearLength", sol::property([](const AircraftComponent& a)
                                                                        { return a.m_GearLength; }, [](AircraftComponent& a, f32 x)
                                                                        { if (std::isfinite(x) && x > 0.0f) a.m_GearLength = x; }),
                                            "gearStiffness", sol::property([](const AircraftComponent& a)
                                                                           { return a.m_GearStiffness; }, [](AircraftComponent& a, f32 x)
                                                                           { if (std::isfinite(x) && x >= 0.0f) a.m_GearStiffness = x; }),
                                            "gearDamping", sol::property([](const AircraftComponent& a)
                                                                         { return a.m_GearDamping; }, [](AircraftComponent& a, f32 x)
                                                                         { if (std::isfinite(x)) a.m_GearDamping = std::clamp(x, 0.0f, 1.0f); }),
                                            "gearRollingResistance", sol::property([](const AircraftComponent& a)
                                                                                   { return a.m_GearRollingResistance; }, [](AircraftComponent& a, f32 x)
                                                                                   { if (std::isfinite(x) && x >= 0.0f) a.m_GearRollingResistance = x; }),
                                            "gearLateralGrip", sol::property([](const AircraftComponent& a)
                                                                             { return a.m_GearLateralGrip; }, [](AircraftComponent& a, f32 x)
                                                                             { if (std::isfinite(x) && x >= 0.0f) a.m_GearLateralGrip = x; }),
                                            "throttleInput", sol::property([](const AircraftComponent& a)
                                                                           { return a.m_ThrottleInput; }, [](AircraftComponent& a, f32 x)
                                                                           { if (std::isfinite(x)) a.m_ThrottleInput = std::clamp(x, 0.0f, 1.0f); }),
                                            "pitchInput", sol::property([](const AircraftComponent& a)
                                                                        { return a.m_PitchInput; }, [](AircraftComponent& a, f32 x)
                                                                        { if (std::isfinite(x)) a.m_PitchInput = std::clamp(x, -1.0f, 1.0f); }),
                                            "rollInput", sol::property([](const AircraftComponent& a)
                                                                       { return a.m_RollInput; }, [](AircraftComponent& a, f32 x)
                                                                       { if (std::isfinite(x)) a.m_RollInput = std::clamp(x, -1.0f, 1.0f); }),
                                            "yawInput", sol::property([](const AircraftComponent& a)
                                                                      { return a.m_YawInput; }, [](AircraftComponent& a, f32 x)
                                                                      { if (std::isfinite(x)) a.m_YawInput = std::clamp(x, -1.0f, 1.0f); }));

        // --- RagdollComponent ---
        // Authored ragdoll tuning. m_Skeleton (a runtime Ref), m_SkeletonEntity,
        // and m_RuntimeRagdollToken are not exposed to scripts: the skeleton link
        // is established by the editor / scene, not from Lua. Mass/radius setters
        // require finite positive values; the swing/twist limits clamp to the
        // [0, 180]-degree range JoltScene::CreateRagdoll enforces.
        lua.new_usertype<RagdollComponent>("RagdollComponent",
                                           "enabled", sol::property([](const RagdollComponent& r)
                                                                    { return r.m_Enabled; }, [](RagdollComponent& r, bool v)
                                                                    { r.m_Enabled = v; }),
                                           "boneMass", sol::property([](const RagdollComponent& r)
                                                                     { return r.m_BoneMass; }, [](RagdollComponent& r, f32 x)
                                                                     { if (std::isfinite(x) && x > 0.0f) r.m_BoneMass = x; }),
                                           "boneRadius", sol::property([](const RagdollComponent& r)
                                                                       { return r.m_BoneRadius; }, [](RagdollComponent& r, f32 x)
                                                                       { if (std::isfinite(x) && x > 0.0f) r.m_BoneRadius = x; }),
                                           "swingLimitDeg", sol::property([](const RagdollComponent& r)
                                                                          { return r.m_SwingLimitDeg; }, [](RagdollComponent& r, f32 x)
                                                                          { if (std::isfinite(x)) r.m_SwingLimitDeg = std::clamp(x, 0.0f, 180.0f); }),
                                           "twistLimitDeg", sol::property([](const RagdollComponent& r)
                                                                          { return r.m_TwistLimitDeg; }, [](RagdollComponent& r, f32 x)
                                                                          { if (std::isfinite(x)) r.m_TwistLimitDeg = std::clamp(x, 0.0f, 180.0f); }));

        // --- ClothComponent (issue #460) ---
        // Authored soft-body parameters. Setters guard finiteness / ranges (mirrors the
        // clamps in JoltShapes::CreateClothSharedSettings) so a script can't feed Jolt a
        // NaN. Changes take effect at the next OnPhysics3DStart (the soft body is rebuilt
        // from these fields), matching how the other physics components behave.
        lua.new_usertype<ClothComponent>("ClothComponent",
                                         "enabled", sol::property([](const ClothComponent& c)
                                                                  { return c.m_Enabled; }, [](ClothComponent& c, bool v)
                                                                  { c.m_Enabled = v; }),
                                         "columns", sol::property([](const ClothComponent& c)
                                                                  { return c.m_Columns; }, [](ClothComponent& c, u32 v)
                                                                  { c.m_Columns = std::clamp(v, 2u, 128u); }),
                                         "rows", sol::property([](const ClothComponent& c)
                                                               { return c.m_Rows; }, [](ClothComponent& c, u32 v)
                                                               { c.m_Rows = std::clamp(v, 2u, 128u); }),
                                         "width", sol::property([](const ClothComponent& c)
                                                                { return c.m_Width; }, [](ClothComponent& c, f32 v)
                                                                { if (std::isfinite(v) && v > 0.0f) c.m_Width = v; }),
                                         "height", sol::property([](const ClothComponent& c)
                                                                 { return c.m_Height; }, [](ClothComponent& c, f32 v)
                                                                 { if (std::isfinite(v) && v > 0.0f) c.m_Height = v; }),
                                         "mass", sol::property([](const ClothComponent& c)
                                                               { return c.m_Mass; }, [](ClothComponent& c, f32 v)
                                                               { if (std::isfinite(v) && v > 0.0f) c.m_Mass = v; }),
                                         "compliance", sol::property([](const ClothComponent& c)
                                                                     { return c.m_Compliance; }, [](ClothComponent& c, f32 v)
                                                                     { if (std::isfinite(v) && v >= 0.0f) c.m_Compliance = v; }),
                                         "bendCompliance", sol::property([](const ClothComponent& c)
                                                                         { return c.m_BendCompliance; }, [](ClothComponent& c, f32 v)
                                                                         { if (std::isfinite(v) && v >= 0.0f) c.m_BendCompliance = v; }),
                                         "linearDamping", sol::property([](const ClothComponent& c)
                                                                        { return c.m_LinearDamping; }, [](ClothComponent& c, f32 v)
                                                                        { if (std::isfinite(v) && v >= 0.0f) c.m_LinearDamping = v; }),
                                         "pressure", sol::property([](const ClothComponent& c)
                                                                   { return c.m_Pressure; }, [](ClothComponent& c, f32 v)
                                                                   { if (std::isfinite(v) && v >= 0.0f) c.m_Pressure = v; }),
                                         "iterations", sol::property([](const ClothComponent& c)
                                                                     { return c.m_Iterations; }, [](ClothComponent& c, u32 v)
                                                                     { c.m_Iterations = std::clamp(v, 1u, 32u); }),
                                         "attachment", sol::property([](const ClothComponent& c)
                                                                     { return static_cast<int>(std::to_underlying(c.m_Attachment)); }, [](ClothComponent& c, int v)
                                                                     { if (v >= static_cast<int>(ClothAttachment::None) && v <= static_cast<int>(ClothAttachment::LeftEdge)) c.m_Attachment = static_cast<ClothAttachment>(v); }),
                                         "windInfluence", sol::property([](const ClothComponent& c)
                                                                        { return c.m_WindInfluence; }, [](ClothComponent& c, f32 v)
                                                                        { if (std::isfinite(v)) c.m_WindInfluence = std::clamp(v, 0.0f, 1.0f); }),
                                         // Skeleton attachment (issue #460 cape slice): the entity (UUID) whose bone the
                                         // cloth's pinned edge follows, and the bone name on it. Take effect at the next
                                         // OnPhysics3DStart (the attachment is resolved when the soft body is (re)built).
                                         "attachmentEntity", sol::property([](const ClothComponent& c)
                                                                           { return static_cast<u64>(c.m_AttachmentEntity); }, [](ClothComponent& c, u64 v)
                                                                           { c.m_AttachmentEntity = UUID(v); }),
                                         "attachmentBone", sol::property([](const ClothComponent& c)
                                                                         { return c.m_AttachmentBone; }, [](ClothComponent& c, const std::string& v)
                                                                         { c.m_AttachmentBone = v; }));

    }
} // namespace OloEngine
