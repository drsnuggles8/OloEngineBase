#include "OloEnginePCH.h"
#include "LuaScriptGlueInternal.h"

// =============================================================================
// LuaScriptGlue_Platform.cpp — audio, video, Steamworks, networking, RPC and input.
//
// One of the LuaScriptGlue_*.cpp parts. See LuaScriptGlueInternal.h for why the
// glue is split and why the call order in RegisterAllTypes is load-bearing.
// =============================================================================

namespace OloEngine
{
    void LuaScriptGlue::RegisterPlatformTypes(sol::state& lua)
    {
        // --- AudioSourceComponent ---
        lua.new_usertype<AudioSourceComponent>("AudioSourceComponent", "volume", sol::property([](const AudioSourceComponent& c)
                                                                                               { return c.GetConfig().VolumeMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                                               {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.0f, 2.0f);
                    c.GetConfig().VolumeMultiplier = v;
                    if (c.Source) { c.Source->SetVolume(v); } }),
                                               "pitch", sol::property([](const AudioSourceComponent& c)
                                                                      { return c.GetConfig().PitchMultiplier; }, [](AudioSourceComponent& c, f32 v)
                                                                      {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.1f, 3.0f);
                    c.GetConfig().PitchMultiplier = v;
                    if (c.Source) { c.Source->SetPitch(v); } }),
                                               "playOnAwake", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.GetConfig().PlayOnAwake; }, [](AudioSourceComponent& c, bool v)
                                                                            { c.GetConfig().PlayOnAwake = v; }),
                                               "looping", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.GetConfig().Looping; }, [](AudioSourceComponent& c, bool v)
                                                                        {
                    c.GetConfig().Looping = v;
                    if (c.Source) { c.Source->SetLooping(v); } }),
                                               "spatialization", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.GetConfig().Spatialization; }, [](AudioSourceComponent& c, bool v)
                                                                               {
                    c.GetConfig().Spatialization = v;
                    if (c.Source) { c.Source->SetSpatialization(v); } }),
                                               // Voice-budget importance in [0, 1] (issue #730). Higher survives stealing.
                                               "priority", sol::property([](const AudioSourceComponent& c)
                                                                         { return c.GetConfig().Priority; }, [](AudioSourceComponent& c, f32 v)
                                                                         {
                    // Documented as [0, 1] on the line above, so hold it to that:
                    // a NaN here silently loses every voice-stealing comparison
                    // it takes part in, which reads as a sound that never plays.
                    if (!std::isfinite(v)) { return; }
                    v = std::clamp(v, 0.0f, 1.0f);
                    c.GetConfig().Priority = v;
                    if (c.Source) { c.Source->SetPriority(v); } }),
                                               // True while registered with the voice budget but inaudible because
                                               // higher-scoring voices hold every slot. `isPlaying` stays true here —
                                               // the sound is logically running, just silent.
                                               "isVirtualized", [](const AudioSourceComponent& c) -> bool
                                               { return c.Source && c.Source->IsVirtualized(); }, "useEventSystem", sol::property([](const AudioSourceComponent& c)
                                                                                                                                  { return c.GetUseEventSystem(); }, [](AudioSourceComponent& c, bool v)
                                                                                                                                  { c.SetUseEventSystem(v); }),
                                               "startEvent", sol::property([](const AudioSourceComponent& c)
                                                                           { return c.GetStartEvent(); }, [](AudioSourceComponent& c, const std::string& v)
                                                                           {
                    c.SetStartEvent(v);
                    c.SetStartCommandID(Audio::CommandID::FromString(v)); }),
                                               "isPlaying", [](const AudioSourceComponent& c) -> bool
                                               {
                if (c.GetUseEventSystem() && c.ActiveEventID != 0)
                {
                    return Audio::AudioPlayback::IsEventActive(c.ActiveEventID);
                }
                return c.Source && c.Source->IsPlaying(); }, "Play", [](AudioSourceComponent& c, sol::optional<u64> ownerUUID)
                                               {
                if (c.GetUseEventSystem() && c.GetStartCommandID().IsValid())
                {
                    if (c.ActiveEventID != 0)
                    {
                        Audio::AudioPlayback::StopEvent(c.ActiveEventID);
                    }
                    u64 objectID = ownerUUID.value_or(0);
                    c.ActiveEventID = Audio::AudioPlayback::PostTrigger(c.GetStartCommandID(), objectID);
                    return;
                }
                if (c.Source) { c.Source->Play(); } }, "Stop", [](AudioSourceComponent& c)
                                               {
                if (c.GetUseEventSystem() && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::StopEvent(c.ActiveEventID);
                    c.ActiveEventID = 0;
                    return;
                }
                if (c.Source) { c.Source->Stop(); } }, "Pause", [](AudioSourceComponent& c)
                                               {
                if (c.GetUseEventSystem() && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::PauseEvent(c.ActiveEventID);
                    return;
                }
                if (c.Source) { c.Source->Pause(); } }, "UnPause", [](AudioSourceComponent& c)
                                               {
                if (c.GetUseEventSystem() && c.ActiveEventID != 0)
                {
                    Audio::AudioPlayback::ResumeEvent(c.ActiveEventID);
                    return;
                }
                if (c.Source) { c.Source->UnPause(); } },
                                               // --- Spatial audio properties ---
                                               "attenuationModel", sol::property([](const AudioSourceComponent& c)
                                                                                 { return std::to_underlying(c.GetConfig().AttenuationModel); }, [](AudioSourceComponent& c, int v)
                                                                                 {
                    if (v < 0 || v > std::to_underlying(AttenuationModelType::Exponential)) return;
                    c.GetConfig().AttenuationModel = static_cast<AttenuationModelType>(v);
                    if (c.Source) { c.Source->SetAttenuationModel(c.GetConfig().AttenuationModel); } }),
                                               "rollOff", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.GetConfig().RollOff; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().RollOff = v;
                    if (c.Source) { c.Source->SetRollOff(v); } }),
                                               "minGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.GetConfig().MinGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 0.0f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().MinGain = v;
                    if (c.GetConfig().MinGain > c.GetConfig().MaxGain) c.GetConfig().MaxGain = c.GetConfig().MinGain;
                    if (c.Source) { c.Source->SetMinGain(c.GetConfig().MinGain); c.Source->SetMaxGain(c.GetConfig().MaxGain); } }),
                                               "maxGain", sol::property([](const AudioSourceComponent& c)
                                                                        { return c.GetConfig().MaxGain; }, [](AudioSourceComponent& c, f32 v)
                                                                        {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::clamp(v, 0.0f, 1.0f);
                    c.GetConfig().MaxGain = v;
                    if (c.GetConfig().MinGain > c.GetConfig().MaxGain) c.GetConfig().MinGain = c.GetConfig().MaxGain;
                    if (c.Source) { c.Source->SetMinGain(c.GetConfig().MinGain); c.Source->SetMaxGain(c.GetConfig().MaxGain); } }),
                                               "minDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.GetConfig().MinDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            {
                    if (!std::isfinite(v)) v = 0.3f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().MinDistance = v;
                    if (c.GetConfig().MinDistance > c.GetConfig().MaxDistance) c.GetConfig().MaxDistance = c.GetConfig().MinDistance;
                    if (c.Source) { c.Source->SetMinDistance(v); c.Source->SetMaxDistance(c.GetConfig().MaxDistance); } }),
                                               "maxDistance", sol::property([](const AudioSourceComponent& c)
                                                                            { return c.GetConfig().MaxDistance; }, [](AudioSourceComponent& c, f32 v)
                                                                            {
                    if (!std::isfinite(v)) v = 1000.0f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().MaxDistance = v;
                    if (c.GetConfig().MinDistance > c.GetConfig().MaxDistance) c.GetConfig().MinDistance = c.GetConfig().MaxDistance;
                    if (c.Source) { c.Source->SetMaxDistance(v); c.Source->SetMinDistance(c.GetConfig().MinDistance); } }),
                                               "coneInnerAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.GetConfig().ConeInnerAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               {
                    if (!std::isfinite(v)) v = glm::radians(360.0f);
                    v = std::clamp(v, 0.0f, glm::radians(360.0f));
                    c.GetConfig().ConeInnerAngle = v;
                    if (c.Source) { c.Source->SetCone(c.GetConfig().ConeInnerAngle, c.GetConfig().ConeOuterAngle, c.GetConfig().ConeOuterGain); } }),
                                               "coneOuterAngle", sol::property([](const AudioSourceComponent& c)
                                                                               { return c.GetConfig().ConeOuterAngle; }, [](AudioSourceComponent& c, f32 v)
                                                                               {
                    if (!std::isfinite(v)) v = glm::radians(360.0f);
                    v = std::clamp(v, 0.0f, glm::radians(360.0f));
                    c.GetConfig().ConeOuterAngle = v;
                    if (c.Source) { c.Source->SetCone(c.GetConfig().ConeInnerAngle, c.GetConfig().ConeOuterAngle, c.GetConfig().ConeOuterGain); } }),
                                               "coneOuterGain", sol::property([](const AudioSourceComponent& c)
                                                                              { return c.GetConfig().ConeOuterGain; }, [](AudioSourceComponent& c, f32 v)
                                                                              {
                    if (!std::isfinite(v)) v = 0.0f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().ConeOuterGain = v;
                    if (c.Source) { c.Source->SetCone(c.GetConfig().ConeInnerAngle, c.GetConfig().ConeOuterAngle, c.GetConfig().ConeOuterGain); } }),
                                               "SetCone", [](AudioSourceComponent& c, f32 innerAngle, f32 outerAngle, f32 outerGain)
                                               {
                    if (!std::isfinite(innerAngle)) { innerAngle = glm::radians(360.0f); }
                    if (!std::isfinite(outerAngle)) { outerAngle = glm::radians(360.0f); }
                    if (!std::isfinite(outerGain)) { outerGain = 0.0f; }
                    innerAngle = std::clamp(innerAngle, 0.0f, glm::radians(360.0f));
                    outerAngle = std::clamp(outerAngle, 0.0f, glm::radians(360.0f));
                    outerGain = std::max(outerGain, 0.0f);
                    c.GetConfig().ConeInnerAngle = innerAngle;
                    c.GetConfig().ConeOuterAngle = outerAngle;
                    c.GetConfig().ConeOuterGain = outerGain;
                    if (c.Source) { c.Source->SetCone(innerAngle, outerAngle, outerGain); } }, "dopplerFactor", sol::property([](const AudioSourceComponent& c)
                                                                                   { return c.GetConfig().DopplerFactor; }, [](AudioSourceComponent& c, f32 v)
                                                                                   {
                    if (!std::isfinite(v)) v = 1.0f;
                    v = std::max(v, 0.0f);
                    c.GetConfig().DopplerFactor = v;
                    if (c.Source) { c.Source->SetDopplerFactor(v); } }));

        // --- AudioListenerComponent ---
        lua.new_usertype<AudioListenerComponent>("AudioListenerComponent",
                                                 "active", &AudioListenerComponent::Active);

        // --- AudioSoundGraphComponent ---
        // The graph runtime (`Sound`) is allocated by Scene::InitAudioRuntime after the asset
        // compiles. Before that the actions silently no-op and `isPlaying` returns false, so
        // Lua scripts can poll without crashing on early-frame calls.
        lua.new_usertype<AudioSoundGraphComponent>("AudioSoundGraphComponent", "volume", sol::property([](const AudioSoundGraphComponent& c)
                                                                                                       { return c.VolumeMultiplier; }, [](AudioSoundGraphComponent& c, f32 v)
                                                                                                       {
                if (!std::isfinite(v)) v = 1.0f;
                v = std::clamp(v, 0.0f, 2.0f);
                c.VolumeMultiplier = v; }),
                                                   "pitch", sol::property([](const AudioSoundGraphComponent& c)
                                                                          { return c.PitchMultiplier; }, [](AudioSoundGraphComponent& c, f32 v)
                                                                          {
                if (!std::isfinite(v)) v = 1.0f;
                v = std::clamp(v, 0.1f, 3.0f);
                c.PitchMultiplier = v; }),
                                                   "looping", &AudioSoundGraphComponent::Looping, "playOnAwake", &AudioSoundGraphComponent::PlayOnAwake, "isPlaying", [](const AudioSoundGraphComponent& c) -> bool
                                                   { return c.Sound && c.Sound->IsPlaying(); }, "Play", [](AudioSoundGraphComponent& c)
                                                   { if (c.Sound) c.Sound->Play(); }, "Stop", [](AudioSoundGraphComponent& c)
                                                   { if (c.Sound) c.Sound->Stop(); }, "Pause", [](AudioSoundGraphComponent& c)
                                                   { if (c.Sound) c.Sound->Pause(); },
                                                   // Single Lua `SetParameter` dispatches across f32 / i32 / bool via sol::overload.
                                                   "SetParameter", sol::overload([](AudioSoundGraphComponent& c, const std::string& name, f32 value)
                                                                                 { return c.SetParameter(name, value); }, [](AudioSoundGraphComponent& c, const std::string& name, i32 value)
                                                                                 { return c.SetParameter(name, value); }, [](AudioSoundGraphComponent& c, const std::string& name, bool value)
                                                                                 { return c.SetParameter(name, value); }));

        // --- VideoOverlayComponent (fullscreen cutscene overlay) ---
        // The runtime Player is created by VideoSystem on the first runtime tick; before
        // then the actions no-op and isPlaying returns false, so scripts can poll safely.
        lua.new_usertype<VideoOverlayComponent>("VideoOverlayComponent", "videoPath", &VideoOverlayComponent::VideoPath, "playOnStart", &VideoOverlayComponent::PlayOnStart, "skipOnInput", &VideoOverlayComponent::SkipOnInput, "looping", &VideoOverlayComponent::Looping, "volume", sol::property([](const VideoOverlayComponent& c)
                                                                                                                                                                                                                                                                                                     { return c.Volume; }, [](VideoOverlayComponent& c, f32 v)
                                                                                                                                                                                                                                                                                                     {
                    if (!std::isfinite(v)) v = 1.0f;
                    c.Volume = std::clamp(v, 0.0f, 1.0f); }),
                                                "isPlaying", [](const VideoOverlayComponent& c) -> bool
                                                { return c.Player && c.Player->IsPlaying(); }, "isFinished", [](const VideoOverlayComponent& c) -> bool
                                                { return c.Player && c.Player->IsFinished(); }, "Play", [](VideoOverlayComponent& c)
                                                { if (c.Player) c.Player->Play(); }, "Pause", [](VideoOverlayComponent& c)
                                                { if (c.Player) c.Player->Pause(); }, "Stop", [](VideoOverlayComponent& c)
                                                { if (c.Player) c.Player->Stop(); });

        // --- VideoSurfaceComponent (world-space video on a mesh) ---
        lua.new_usertype<VideoSurfaceComponent>("VideoSurfaceComponent", "videoPath", &VideoSurfaceComponent::VideoPath, "autoPlay", &VideoSurfaceComponent::AutoPlay, "looping", &VideoSurfaceComponent::Looping, "volume", sol::property([](const VideoSurfaceComponent& c)
                                                                                                                                                                                                                                           { return c.Volume; }, [](VideoSurfaceComponent& c, f32 v)
                                                                                                                                                                                                                                           {
                    if (!std::isfinite(v)) v = 1.0f;
                    c.Volume = std::clamp(v, 0.0f, 1.0f); }),
                                                "isPlaying", [](const VideoSurfaceComponent& c) -> bool
                                                { return c.Player && c.Player->IsPlaying(); }, "Play", [](VideoSurfaceComponent& c)
                                                { if (c.Player) c.Player->Play(); }, "Pause", [](VideoSurfaceComponent& c)
                                                { if (c.Player) c.Player->Pause(); }, "Stop", [](VideoSurfaceComponent& c)
                                                { if (c.Player) c.Player->Stop(); });

        // --- Video (global table) — fullscreen cutscene control. ---
        auto videoTable = lua.create_named_table("Video");
        videoTable["PlayFullscreen"] = sol::overload(
            [](const std::string& path)
            { VideoSystem::PlayFullscreen(path, false, {}); },
            [](const std::string& path, bool loop)
            { VideoSystem::PlayFullscreen(path, loop, {}); },
            [](const std::string& path, sol::function onFinished)
            {
                VideoSystem::PlayFullscreen(path, false, [onFinished]()
                                            {
                    // Invoke as a protected_function so a Lua error in the callback is logged
                    // rather than thrown into the C++ caller (OnFinished runs during the scene tick).
                    sol::protected_function pf = onFinished;
                    if (!pf.valid())
                        return;
                    sol::protected_function_result result = pf();
                    if (!result.valid())
                    {
                        sol::error err = result;
                        OLO_ERROR("Video.PlayFullscreen onFinished callback error: {}", err.what());
                    } });
            });
        videoTable["Stop"] = []()
        { VideoSystem::StopFullscreen(); };
        videoTable["Skip"] = []()
        { VideoSystem::SkipFullscreen(); };
        videoTable["IsPlaying"] = []()
        { return VideoSystem::IsFullscreenPlaying(); };
        videoTable["SetSkippable"] = [](bool skippable)
        { VideoSystem::SetFullscreenSkippable(skippable); };

        // --- AudioEvents (global table) ---
        auto audioEventsTable = lua.create_named_table("AudioEvents");
        audioEventsTable["PostTrigger"] = [](const std::string& eventName, u64 objectID) -> u64
        {
            return Audio::AudioPlayback::PostTriggerByName(eventName, objectID);
        };
        audioEventsTable["StopEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::StopEvent(eventID);
        };
        audioEventsTable["PauseEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::PauseEvent(eventID);
        };
        audioEventsTable["ResumeEvent"] = [](u64 eventID)
        {
            Audio::AudioPlayback::ResumeEvent(eventID);
        };
        audioEventsTable["StopAll"] = []()
        {
            Audio::AudioPlayback::StopAll();
        };
        audioEventsTable["IsEventActive"] = [](u64 eventID) -> bool
        {
            return Audio::AudioPlayback::IsEventActive(eventID);
        };

        // --- Steamworks platform services (#644) ---
        //
        // Self-contained block; see the coverage test at
        // tests/Platform/Steam/SteamScriptBindingCoverageTest.cpp, which asserts every function
        // here exists and is callable with Steam absent.
        //
        // EVERY function below is safe to call when Steam is unavailable — not compiled in, or
        // compiled in with no Steam client running. They return false / "" / an empty table
        // rather than erroring, so a game script can call Steam unconditionally and simply do
        // nothing on a machine without it. That is the whole degradation contract, expressed at
        // the script layer.
        auto steamTable = lua.create_named_table("Steam");

        // Gate UI and skip work on this; you do NOT need it to guard the calls below.
        steamTable.set_function("isAvailable", &SteamManager::IsAvailable);
        steamTable.set_function("getAppID", &SteamManager::GetAppId);
        steamTable.set_function("getPersonaName", &SteamManager::GetPersonaName);

        // Steam.unlockAchievement("ACH_WIN_ONE_GAME") -> bool
        //
        // Returns true when the achievement is unlocked as a result of this call OR was already
        // unlocked, because that is what a gameplay script means by "make sure this is
        // unlocked". Scripts that need to distinguish a fresh unlock (to play a sting, say)
        // should check isAchievementUnlocked() first.
        steamTable.set_function("unlockAchievement", [](const std::string& achievementId) -> bool
                                { return SteamSucceeded(SteamManager::UnlockAchievement(achievementId)); });
        steamTable.set_function("clearAchievement", [](const std::string& achievementId) -> bool
                                { return SteamSucceeded(SteamManager::ClearAchievement(achievementId)); });
        steamTable.set_function("isAchievementUnlocked", [](const std::string& achievementId) -> bool
                                { return SteamManager::IsAchievementUnlocked(achievementId); });
        steamTable.set_function("storeStats", []() -> bool
                                { return SteamSucceeded(SteamManager::StoreStats()); });

        // Steam.setRichPresence("status", "Exploring the caves")
        //
        // An empty value clears that key, matching Steam's own semantics. Over-long keys or
        // values are REJECTED (returning false) rather than truncated — a silently shortened
        // presence string is far harder to notice than a false return.
        steamTable.set_function("setRichPresence", [](const std::string& key, const std::string& value) -> bool
                                { return SteamSucceeded(SteamManager::SetRichPresence(key, value)); });
        steamTable.set_function("clearRichPresence", &SteamManager::ClearRichPresence);

        // True while the Steam overlay is displayed. Games typically pause on the rising edge.
        // Verified on the OpenGL backend only — no claim is made about Vulkan.
        steamTable.set_function("isOverlayActive", &SteamManager::IsOverlayActive);

        // --- Steam Cloud ---
        //
        // Exposed as strings rather than byte tables: scripts store small JSON/text blobs, and
        // the engine's own save-games go through SaveGameManager, not through here.
        steamTable.set_function("isCloudEnabled", &SteamManager::IsCloudEnabled);

        steamTable.set_function("cloudWrite",
                                [](const std::string& name, const std::string& contents) -> bool
                                {
                                    const std::span<const u8> bytes{
                                        reinterpret_cast<const u8*>(contents.data()), contents.size()
                                    };
                                    return SteamSucceeded(SteamManager::CloudWrite(name, bytes));
                                });

        // Returns nil when the file is absent or Cloud is unavailable, so scripts can write
        // `local data = Steam.cloudRead("x") or default`.
        steamTable.set_function("cloudRead",
                                [](const std::string& name) -> sol::optional<std::string>
                                {
                                    std::vector<u8> bytes;
                                    if (!SteamSucceeded(SteamManager::CloudRead(name, bytes)))
                                    {
                                        return sol::nullopt;
                                    }
                                    // Iterator pair, not (data(), size()): on an empty vector
                                    // data() may be null, and std::string(nullptr, 0) is UB even
                                    // though the length is zero. The iterator form is well-defined
                                    // for an empty range and reads the same.
                                    return std::string(bytes.begin(), bytes.end());
                                });

        steamTable.set_function("cloudExists", [](const std::string& name) -> bool
                                { return SteamManager::CloudExists(name); });
        steamTable.set_function("cloudDelete", [](const std::string& name) -> bool
                                { return SteamSucceeded(SteamManager::CloudDelete(name)); });

        // 1-based array of file names, matching Lua convention. Empty when Cloud is unavailable.
        // sol::this_state rather than capturing `lua`, matching every other table-returning
        // binding in this file (e.g. SaveGame.EnumerateSaves) — it takes the state from the
        // calling context instead of holding a reference for the lifetime of the binding.
        steamTable.set_function("cloudEnumerate",
                                [](sol::this_state s) -> sol::table
                                {
                                    OLO_PROFILE_SCOPE("Lua::Steam::CloudEnumerate");
                                    sol::state_view luaState(s);
                                    const std::vector<std::string> files = SteamManager::CloudEnumerate();
                                    sol::table result = luaState.create_table(static_cast<int>(files.size()), 0);
                                    int index = 1;
                                    for (const std::string& name : files)
                                    {
                                        result[index++] = name;
                                    }
                                    return result;
                                });

        // Returns total, available (both 0 when unavailable).
        steamTable.set_function("getCloudQuota",
                                []() -> std::tuple<u64, u64>
                                {
                                    SteamCloudQuota quota;
                                    if (!SteamSucceeded(SteamManager::GetCloudQuota(quota)))
                                    {
                                        return { 0ull, 0ull };
                                    }
                                    return { quota.TotalBytes, quota.AvailableBytes };
                                });

        // --- NetworkManager (static functions as table) ---
        auto networkTable = lua.create_named_table("Network");
        networkTable.set_function("isServer", &NetworkManager::IsServer);
        networkTable.set_function("isClient", &NetworkManager::IsClient);
        networkTable.set_function("isConnected", &NetworkManager::IsConnected);
        networkTable.set_function("connect", &NetworkManager::Connect);
        networkTable.set_function("disconnect", &NetworkManager::Disconnect);
        networkTable.set_function("startServer", &NetworkManager::StartServer);
        networkTable.set_function("stopServer", &NetworkManager::StopServer);

        // The id the server assigned this client (0 before the assignment lands, or
        // on a pure server). Scripts need it to tell "my pawn" from everyone else's.
        networkTable.set_function("getLocalClientID", []() -> u32
                                  { return NetworkManager::GetClientDriver().GetLocalClientID(); });
        networkTable.set_function("getCurrentTick", &NetworkManager::GetCurrentTick);

        // The pawn this client owns and predicts. 0 before the server has spawned
        // one (or on a pure server).
        networkTable.set_function("getLocalPlayerEntity",
                                  []() -> u64
                                  {
                                      Scene* scene = NetworkManager::GetActiveScene();
                                      if (scene == nullptr)
                                      {
                                          return 0;
                                      }
                                      return NetworkManager::GetClientDriver().FindLocalPlayerEntity(*scene);
                                  });

        // Install the engine's built-in movement input command on BOTH the client
        // prediction path and the server's authoritative path — one function, so the
        // predicted and authoritative results cannot drift. `maxStep` bounds a
        // single command's displacement server-side.
        networkTable.set_function("useMovementInput",
                                  [](sol::optional<f32> maxStep)
                                  {
                                      const f32 clamp = maxStep.value_or(1.0f);
                                      if (!std::isfinite(clamp) || clamp < 0.0f)
                                      {
                                          OLO_CORE_WARN_TAG("Networking",
                                                            "Network.useMovementInput: invalid maxStep {}", clamp);
                                          return;
                                      }
                                      NetworkManager::SetInputApplyCallback(MakeMovementApplyCallback(clamp));
                                  });

        // Send one movement step. The displacement is baked here (direction × speed
        // × dt) rather than on the server, because reconciliation replays these
        // commands with no timeline of their own — see NetworkMovementInput.
        networkTable.set_function("sendMoveInput",
                                  [](u64 entityID, f32 x, f32 y, f32 z)
                                  {
                                      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                                      {
                                          OLO_CORE_WARN_TAG("Networking",
                                                            "Network.sendMoveInput: non-finite displacement ignored");
                                          return;
                                      }
                                      NetworkMovementInput input;
                                      input.Delta = { x, y, z };
                                      NetworkManager::SendInput(entityID, input.Encode());
                                  });

        // Server-side entity lifecycle. Returns the new entity's UUID, or 0 if
        // there is no active scene.
        networkTable.set_function("spawn",
                                  [](const std::string& archetype, const std::string& name, u32 ownerClientID,
                                     sol::optional<int> authority) -> u64
                                  {
                                      const int rawAuthority = authority.value_or(
                                          static_cast<int>(ENetworkAuthority::Server));
                                      if (rawAuthority < 0 || rawAuthority > static_cast<int>(ENetworkAuthority::Shared))
                                      {
                                          OLO_CORE_WARN_TAG("Networking", "Network.spawn: invalid authority {}", rawAuthority);
                                          return 0;
                                      }
                                      // Deferred: the id is valid now, the entity
                                      // materialises at the next network tick. See
                                      // NetworkManager::SpawnReplicated.
                                      return NetworkManager::SpawnReplicated(
                                          archetype, name, ownerClientID, static_cast<ENetworkAuthority>(rawAuthority));
                                  });
        networkTable.set_function("despawn", [](u64 entityID)
                                  { NetworkManager::DespawnReplicated(entityID); });

        // Client-side input submission. `data` is an array of byte values (0-255);
        // the server applies the identical bytes through the same InputApplyCallback,
        // so the predicted and authoritative results agree by construction.
        networkTable.set_function("sendInput",
                                  [](u64 entityID, sol::table data)
                                  {
                                      std::vector<u8> bytes;
                                      bytes.reserve(data.size());
                                      for (sizet i = 1; i <= data.size(); ++i)
                                      {
                                          sol::optional<int> value = data[i];
                                          if (!value.has_value() || *value < 0 || *value > 255)
                                          {
                                              OLO_CORE_WARN_TAG("Networking",
                                                                "Network.sendInput: element {} is not a byte (0-255)", i);
                                              return;
                                          }
                                          bytes.push_back(static_cast<u8>(*value));
                                      }
                                      NetworkManager::SendInput(entityID, std::move(bytes));
                                  });

        // --- RPC ---
        //
        //   Network.registerRPC("Fire", { target = "server", reliability = "reliable",
        //                                 requiresOwnership = true },
        //                       function(ctx, args) ... end)
        //   Network.invokeRPC("Fire", myEntityId, { 1, "hello", true })
        //
        // Both ends must register the same name: the wire carries only its hash, so
        // an unregistered name is dropped rather than guessed at. Authority is the
        // registry's business, not the caller's — a client that invokes a
        // client-target or multicast RPC is refused at both ends.
        networkTable.set_function(
            "registerRPC",
            [](const std::string& name, sol::table options, sol::protected_function handler)
            {
                RpcDescriptor descriptor;
                descriptor.Name = name;
                descriptor.Target = ParseRpcTarget(options.get_or<std::string>("target", "server"));
                descriptor.Reliability =
                    ParseRpcReliability(options.get_or<std::string>("reliability", "reliable"));
                descriptor.RequiresOwnership = options.get_or("requiresOwnership", true);
                // A handler owned by the Lua VM must not outlive it — the registry
                // drops the Lua-owned entries when the sol::state is destroyed.
                descriptor.Owner = ERpcOwner::Lua;

                if (handler.valid())
                {
                    descriptor.Handler = [handler](const RpcContext& context, const RpcArgList& args)
                    {
                        sol::state_view view(handler.lua_state());
                        sol::table ctx = view.create_table();
                        ctx["senderClientID"] = context.SenderClientID;
                        ctx["entityID"] = context.EntityUUID;
                        ctx["isServer"] = context.IsServer;

                        sol::table luaArgs = view.create_table();
                        for (sizet i = 0; i < args.size(); ++i)
                        {
                            luaArgs[i + 1] = RpcArgToLua(view, args[i]);
                        }

                        if (sol::protected_function_result result = handler(ctx, luaArgs); !result.valid())
                        {
                            sol::error error = result;
                            OLO_CORE_ERROR("[Lua] RPC handler error: {}", error.what());
                        }
                    };
                }

                NetworkManager::RegisterRPC(std::move(descriptor));
            });

        networkTable.set_function("invokeRPC",
                                  [](const std::string& name, u64 entityID, sol::optional<sol::table> args,
                                     sol::optional<u32> targetClientID) -> bool
                                  {
                                      RpcArgList marshalled;
                                      if (args.has_value())
                                      {
                                          const sol::table& table = *args;
                                          marshalled.reserve(table.size());
                                          for (sizet i = 1; i <= table.size(); ++i)
                                          {
                                              sol::object value = table[i];
                                              auto arg = LuaToRpcArg(value);
                                              if (!arg.has_value())
                                              {
                                                  OLO_CORE_WARN_TAG("Networking",
                                                                    "Network.invokeRPC('{}'): argument {} has an "
                                                                    "unsupported type",
                                                                    name, i);
                                                  return false;
                                              }
                                              marshalled.push_back(std::move(*arg));
                                          }
                                      }
                                      return NetworkManager::InvokeRPC(name, entityID, marshalled,
                                                                       targetClientID.value_or(0));
                                  });

        // --- Input (raw + action mapping) ---
        auto inputTable = lua.create_named_table("Input");
        inputTable["IsKeyDown"] = [](u16 keycode)
        {
            return Input::IsKeyPressed(keycode);
        };
        inputTable["IsKeyJustPressed"] = [](u16 keycode)
        {
            return Input::IsKeyJustPressed(static_cast<KeyCode>(keycode));
        };
        inputTable["IsKeyJustReleased"] = [](u16 keycode)
        {
            return Input::IsKeyJustReleased(static_cast<KeyCode>(keycode));
        };
        inputTable["IsMouseButtonDown"] = [](u16 button)
        {
            return Input::IsMouseButtonPressed(button);
        };
        inputTable["IsActionPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionPressed(name);
        };
        inputTable["IsActionJustPressed"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustPressed(name);
        };
        inputTable["IsActionJustReleased"] = [](const std::string& name)
        {
            return InputActionManager::IsActionJustReleased(name);
        };
        inputTable["GetActionAxisValue"] = [](const std::string& name)
        {
            return InputActionManager::GetActionAxisValue(name);
        };
        // Action-map contexts (gameplay/menu/vehicle). Pass an InputContext.* constant.
        // Switching contexts swaps the active action map and resets transient press state.
        // SetInputContext is a hard switch; Push/Pop nest (e.g. push Menu over Gameplay).
        // The context arrives as a bare integer from script, so it is range-checked
        // before the cast — casting an arbitrary i32 to a scoped enum and handing it
        // to the action map is UB with a plausible-looking value.
        const auto toInputContext = [](i32 context, const char* who) -> sol::optional<InputContextType>
        {
            for (const InputContextType candidate : AllInputContextTypes)
            {
                if (static_cast<i32>(candidate) == context)
                    return candidate;
            }
            OLO_CORE_WARN("[Lua Input] {} got out-of-range InputContext {} — ignored", who, context);
            return sol::nullopt;
        };
        inputTable["SetInputContext"] = [toInputContext](i32 context)
        {
            if (const auto ctx = toInputContext(context, "SetInputContext"))
                InputActionManager::SetInputContext(*ctx);
        };
        inputTable["GetInputContext"] = []() -> i32
        {
            return static_cast<i32>(InputActionManager::GetInputContext());
        };
        inputTable["PushInputContext"] = [](i32 context)
        {
            InputActionManager::PushContext(static_cast<InputContextType>(context));
        };
        inputTable["PopInputContext"] = []() -> bool
        {
            return InputActionManager::PopContext();
        };
        inputTable["GetInputContextDepth"] = []() -> i32
        {
            return static_cast<i32>(InputActionManager::GetContextDepth());
        };
        inputTable["RequestRebindMenu"] = [](i32 context) -> bool
        {
            if (context < 0 || context >= static_cast<i32>(AllInputContextTypes.size()))
            {
                OLO_CORE_WARN("[Lua] Input.RequestRebindMenu received invalid context {} — ignoring.", context);
                return false;
            }
            InputActionManager::RequestRebindMenu(static_cast<InputContextType>(context));
            return true;
        };
        inputTable["GetMousePosition"] = []() -> std::tuple<f32, f32>
        {
            auto pos = Input::GetMousePosition();
            if (Scene* scene = ScriptEngine::GetSceneContext(); scene)
                pos -= scene->GetViewportOffset();
            return { pos.x, pos.y };
        };
        // Cursor capture for FPS mouse-look. Pass a CursorMode.* constant.
        // CursorMode.Locked hides + pins the cursor and makes GetMousePosition
        // report unbounded virtual motion (no window-edge stall).
        inputTable["SetCursorMode"] = [](i32 mode)
        {
            Input::SetCursorMode(static_cast<CursorMode>(mode));
        };
        inputTable["GetCursorMode"] = []() -> i32
        {
            return static_cast<i32>(Input::GetCursorMode());
        };
        inputTable["GetWindowSize"] = []() -> std::tuple<f32, f32>
        {
            if (Scene* scene = ScriptEngine::GetSceneContext(); scene && scene->GetViewportWidth() > 0)
                return { static_cast<f32>(scene->GetViewportWidth()), static_cast<f32>(scene->GetViewportHeight()) };
            auto& window = Application::Get().GetWindow();
            return { static_cast<f32>(window.GetWidth()), static_cast<f32>(window.GetHeight()) };
        };

        // --- Gamepad functions (raw access) ---
        {
            OLO_PROFILE_SCOPE("LuaScriptGlue::RegisterGamepad");
            auto gamepadTable = lua.create_named_table("Gamepad");
            gamepadTable["IsButtonPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustPressed"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustPressed(static_cast<GamepadButton>(button));
            };
            gamepadTable["IsButtonJustReleased"] = [](u8 button, i32 index) -> bool
            {
                if (button >= Gamepad::ButtonCount)
                {
                    return false;
                }
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsButtonJustReleased(static_cast<GamepadButton>(button));
            };
            gamepadTable["GetAxis"] = [](u8 axis, i32 index) -> f32
            {
                if (axis >= Gamepad::AxisCount)
                {
                    return 0.0f;
                }
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetAxis(static_cast<GamepadAxis>(axis)) : 0.0f;
            };
            gamepadTable["GetLeftStick"] = [](i32 index) -> glm::vec2
            {
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetLeftStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["GetRightStick"] = [](i32 index) -> glm::vec2
            {
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp ? gp->GetRightStickDeadzone() : glm::vec2(0.0f);
            };
            gamepadTable["IsConnected"] = [](i32 index) -> bool
            {
                const auto* gp = GamepadManager::GetGamepad(index);
                return gp && gp->IsConnected();
            };
            gamepadTable["GetConnectedCount"] = []() -> i32
            {
                return GamepadManager::GetConnectedCount();
            };

            // --- Gamepad button/axis enum constants ---
            auto gpButtonTable = lua.create_named_table("GamepadButton");
            gpButtonTable["South"] = std::to_underlying(GamepadButton::South);
            gpButtonTable["East"] = std::to_underlying(GamepadButton::East);
            gpButtonTable["West"] = std::to_underlying(GamepadButton::West);
            gpButtonTable["North"] = std::to_underlying(GamepadButton::North);
            gpButtonTable["LeftBumper"] = std::to_underlying(GamepadButton::LeftBumper);
            gpButtonTable["RightBumper"] = std::to_underlying(GamepadButton::RightBumper);
            gpButtonTable["Back"] = std::to_underlying(GamepadButton::Back);
            gpButtonTable["Start"] = std::to_underlying(GamepadButton::Start);
            gpButtonTable["Guide"] = std::to_underlying(GamepadButton::Guide);
            gpButtonTable["LeftThumb"] = std::to_underlying(GamepadButton::LeftThumb);
            gpButtonTable["RightThumb"] = std::to_underlying(GamepadButton::RightThumb);
            gpButtonTable["DPadUp"] = std::to_underlying(GamepadButton::DPadUp);
            gpButtonTable["DPadRight"] = std::to_underlying(GamepadButton::DPadRight);
            gpButtonTable["DPadDown"] = std::to_underlying(GamepadButton::DPadDown);
            gpButtonTable["DPadLeft"] = std::to_underlying(GamepadButton::DPadLeft);

            auto gpAxisTable = lua.create_named_table("GamepadAxis");
            gpAxisTable["LeftX"] = std::to_underlying(GamepadAxis::LeftX);
            gpAxisTable["LeftY"] = std::to_underlying(GamepadAxis::LeftY);
            gpAxisTable["RightX"] = std::to_underlying(GamepadAxis::RightX);
            gpAxisTable["RightY"] = std::to_underlying(GamepadAxis::RightY);
            gpAxisTable["LeftTrigger"] = std::to_underlying(GamepadAxis::LeftTrigger);
            gpAxisTable["RightTrigger"] = std::to_underlying(GamepadAxis::RightTrigger);
        }

        // --- KeyCode constants (auto-generated from OLO_KEY_LIST in KeyCodes.h) ---
        {
            auto keyTable = lua.create_named_table("KeyCode");
            // clang-format off
#define OLO_BIND_KEY(name, val) keyTable[#name] = static_cast<KeyCode>(val);
            OLO_KEY_LIST(OLO_BIND_KEY)
#undef OLO_BIND_KEY
            // clang-format on
        }

        // --- MouseButton constants (auto-generated from OLO_MOUSE_LIST in MouseCodes.h) ---
        {
            auto mouseTable = lua.create_named_table("MouseButton");
            // clang-format off
#define OLO_BIND_MOUSE(name, val) mouseTable[#name] = static_cast<MouseCode>(val);
            OLO_MOUSE_LIST(OLO_BIND_MOUSE)
#undef OLO_BIND_MOUSE
            // clang-format on
        }

        // --- CursorMode constants (for Input.SetCursorMode — FPS mouse capture) ---
        {
            auto cursorTable = lua.create_named_table("CursorMode");
            cursorTable["Normal"] = static_cast<i32>(CursorMode::Normal);
            cursorTable["Hidden"] = static_cast<i32>(CursorMode::Hidden);
            cursorTable["Locked"] = static_cast<i32>(CursorMode::Locked);
        }

        // --- InputContext constants (for Input.SetInputContext — action-map contexts) ---
        {
            auto inputContextTable = lua.create_named_table("InputContext");
            inputContextTable["Gameplay"] = static_cast<i32>(InputContextType::Gameplay);
            inputContextTable["Menu"] = static_cast<i32>(InputContextType::Menu);
            inputContextTable["Vehicle"] = static_cast<i32>(InputContextType::Vehicle);
            inputContextTable["Custom"] = static_cast<i32>(InputContextType::Custom);
        }

        // --- UIButtonState constants (for authored UI controller scripts) ---
        {
            auto buttonStateTable = lua.create_named_table("UIButtonState");
            buttonStateTable["Normal"] = static_cast<i32>(UIButtonState::Normal);
            buttonStateTable["Hovered"] = static_cast<i32>(UIButtonState::Hovered);
            buttonStateTable["Pressed"] = static_cast<i32>(UIButtonState::Pressed);
            buttonStateTable["Disabled"] = static_cast<i32>(UIButtonState::Disabled);
        }
    }
} // namespace OloEngine
