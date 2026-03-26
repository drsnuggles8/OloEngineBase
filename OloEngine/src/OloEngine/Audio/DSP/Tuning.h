#pragma once

// Freeverb tuning values (public domain, Jezar at Dreampoint).
// These values assume 44100 Hz sample rate.
// The constructor of ReverbModel adjusts them for the actual sample rate.

namespace OloEngine::Audio::DSP
{
    inline constexpr int NumCombs = 8;
    inline constexpr int NumAllpasses = 4;
    inline constexpr float Muted = 0.0f;
    inline constexpr float FixedGain = 0.015f;
    inline constexpr float ScaleWet = 3.0f;
    inline constexpr float ScaleDry = 2.0f;
    inline constexpr float ScaleDamp = 0.4f;
    inline constexpr float ScaleRoom = 0.28f;
    inline constexpr float OffsetRoom = 0.7f;
    inline constexpr float InitialRoom = 0.5f;
    inline constexpr float InitialDamp = 0.5f;
    inline constexpr float InitialWet = 1.0f / ScaleWet;
    inline constexpr float InitialDry = 0.0f;
    inline constexpr float InitialWidth = 1.0f;
    inline constexpr float InitialMode = 0.0f;
    inline constexpr float FreezeMode = 0.5f;
    inline constexpr int StereoSpread = 23;

    // Comb filter tuning (sample counts at 44100 Hz)
    inline constexpr int CombTuningL1 = 1116;
    inline constexpr int CombTuningR1 = 1116 + StereoSpread;
    inline constexpr int CombTuningL2 = 1188;
    inline constexpr int CombTuningR2 = 1188 + StereoSpread;
    inline constexpr int CombTuningL3 = 1277;
    inline constexpr int CombTuningR3 = 1277 + StereoSpread;
    inline constexpr int CombTuningL4 = 1356;
    inline constexpr int CombTuningR4 = 1356 + StereoSpread;
    inline constexpr int CombTuningL5 = 1422;
    inline constexpr int CombTuningR5 = 1422 + StereoSpread;
    inline constexpr int CombTuningL6 = 1491;
    inline constexpr int CombTuningR6 = 1491 + StereoSpread;
    inline constexpr int CombTuningL7 = 1557;
    inline constexpr int CombTuningR7 = 1557 + StereoSpread;
    inline constexpr int CombTuningL8 = 1617;
    inline constexpr int CombTuningR8 = 1617 + StereoSpread;

    // Allpass tuning (sample counts at 44100 Hz)
    inline constexpr int AllpassTuningL1 = 556;
    inline constexpr int AllpassTuningR1 = 556 + StereoSpread;
    inline constexpr int AllpassTuningL2 = 441;
    inline constexpr int AllpassTuningR2 = 441 + StereoSpread;
    inline constexpr int AllpassTuningL3 = 341;
    inline constexpr int AllpassTuningR3 = 341 + StereoSpread;
    inline constexpr int AllpassTuningL4 = 225;
    inline constexpr int AllpassTuningR4 = 225 + StereoSpread;
} // namespace OloEngine::Audio::DSP
