#pragma once

// =============================================================================
// NodeDescriptions.h — DESCRIBE_NODE specializations for every SoundGraph node
//
// These were previously in NodeDescriptions.cpp, which is a *separate TU* from
// NodeTypes.cpp where `EndpointUtilities::RegisterEndpoints<Node>(...)` is
// actually instantiated. Template specializations of `NodeDescription<Node>`
// have to be VISIBLE at the point of instantiation, otherwise
// `IsDescribedNode_v<Node>` falls back to the primary template's
// `std::false_type` and the entire reflection-driven endpoint registration
// silently short-circuits. The graph then has no InputRefs / OutputSources
// entries, and EstablishConnections fails (with a warning) the moment any
// node-to-graph wire is resolved at runtime.
//
// Putting the specializations in a header (and #including it from NodeTypes.cpp
// before INIT_ENDPOINTS_FUNCS) makes them visible at the right point. The
// static-inline const members the DESCRIBE_NODE macro expands to are C++17
// inline variables, so multiple TUs including this header is ODR-safe.
// =============================================================================

#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GeneratorNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MathNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/EnvelopeNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriggerNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ArrayNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MusicNodes.h"

// NOTE: DESCRIBE_NODE macros must be at global scope (outside any namespace)
// because they expand to template specializations in OloEngine::Core::Reflection.

namespace sg = OloEngine::Audio::SoundGraph;

//==============================================================================
// WavePlayer Node Description
//==============================================================================
DESCRIBE_NODE(sg::WavePlayer,
              NODE_INPUTS(
                  &sg::WavePlayer::m_WaveAsset,
                  &sg::WavePlayer::m_StartTime,
                  &sg::WavePlayer::m_Loop,
                  &sg::WavePlayer::m_NumberOfLoops),
              NODE_OUTPUTS(
                  &sg::WavePlayer::m_OutLeft,
                  &sg::WavePlayer::m_OutRight,
                  &sg::WavePlayer::m_OnPlay,
                  &sg::WavePlayer::m_OnStop,
                  &sg::WavePlayer::m_OnFinished));

//==============================================================================
// Generator Nodes Descriptions
//==============================================================================
DESCRIBE_NODE(sg::SineOscillator,
              NODE_INPUTS(
                  &sg::SineOscillator::m_Frequency,
                  &sg::SineOscillator::m_Amplitude,
                  &sg::SineOscillator::m_Phase),
              NODE_OUTPUTS(
                  &sg::SineOscillator::m_OutValue));

DESCRIBE_NODE(sg::SquareOscillator,
              NODE_INPUTS(
                  &sg::SquareOscillator::m_Frequency,
                  &sg::SquareOscillator::m_Amplitude,
                  &sg::SquareOscillator::m_Phase,
                  &sg::SquareOscillator::m_PulseWidth),
              NODE_OUTPUTS(
                  &sg::SquareOscillator::m_OutValue));

DESCRIBE_NODE(sg::SawtoothOscillator,
              NODE_INPUTS(
                  &sg::SawtoothOscillator::m_Frequency,
                  &sg::SawtoothOscillator::m_Amplitude,
                  &sg::SawtoothOscillator::m_Phase),
              NODE_OUTPUTS(
                  &sg::SawtoothOscillator::m_OutValue));

DESCRIBE_NODE(sg::TriangleOscillator,
              NODE_INPUTS(
                  &sg::TriangleOscillator::m_Frequency,
                  &sg::TriangleOscillator::m_Amplitude,
                  &sg::TriangleOscillator::m_Phase),
              NODE_OUTPUTS(
                  &sg::TriangleOscillator::m_OutValue));

DESCRIBE_NODE(sg::Noise,
              NODE_INPUTS(
                  &sg::Noise::m_Seed,
                  &sg::Noise::m_Type,
                  &sg::Noise::m_Amplitude),
              NODE_OUTPUTS(
                  &sg::Noise::m_OutValue));

//==============================================================================
// Math Nodes Descriptions - Template Specializations
//==============================================================================

// Float specializations
DESCRIBE_NODE(sg::Add<f32>,
              NODE_INPUTS(
                  &sg::Add<f32>::m_Value1,
                  &sg::Add<f32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Add<f32>::m_Out));

DESCRIBE_NODE(sg::Subtract<f32>,
              NODE_INPUTS(
                  &sg::Subtract<f32>::m_Value1,
                  &sg::Subtract<f32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Subtract<f32>::m_Out));

DESCRIBE_NODE(sg::Multiply<f32>,
              NODE_INPUTS(
                  &sg::Multiply<f32>::m_Value,
                  &sg::Multiply<f32>::m_Multiplier),
              NODE_OUTPUTS(
                  &sg::Multiply<f32>::m_Out));

DESCRIBE_NODE(sg::Divide<f32>,
              NODE_INPUTS(
                  &sg::Divide<f32>::m_Value,
                  &sg::Divide<f32>::m_Denominator),
              NODE_OUTPUTS(
                  &sg::Divide<f32>::m_Out));

DESCRIBE_NODE(sg::Min<f32>,
              NODE_INPUTS(
                  &sg::Min<f32>::m_Value1,
                  &sg::Min<f32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Min<f32>::m_Out));

DESCRIBE_NODE(sg::Max<f32>,
              NODE_INPUTS(
                  &sg::Max<f32>::m_Value1,
                  &sg::Max<f32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Max<f32>::m_Out));

DESCRIBE_NODE(sg::Clamp<f32>,
              NODE_INPUTS(
                  &sg::Clamp<f32>::m_Value,
                  &sg::Clamp<f32>::m_MinValue,
                  &sg::Clamp<f32>::m_MaxValue),
              NODE_OUTPUTS(
                  &sg::Clamp<f32>::m_Out));

DESCRIBE_NODE(sg::MapRange<f32>,
              NODE_INPUTS(
                  &sg::MapRange<f32>::m_Value,
                  &sg::MapRange<f32>::m_FromMin,
                  &sg::MapRange<f32>::m_FromMax,
                  &sg::MapRange<f32>::m_ToMin,
                  &sg::MapRange<f32>::m_ToMax),
              NODE_OUTPUTS(
                  &sg::MapRange<f32>::m_Out));

DESCRIBE_NODE(sg::Power<f32>,
              NODE_INPUTS(
                  &sg::Power<f32>::m_Base,
                  &sg::Power<f32>::m_Exponent),
              NODE_OUTPUTS(
                  &sg::Power<f32>::m_Out));

DESCRIBE_NODE(sg::Abs<f32>,
              NODE_INPUTS(
                  &sg::Abs<f32>::m_Value),
              NODE_OUTPUTS(
                  &sg::Abs<f32>::m_Out));

// Integer specializations
DESCRIBE_NODE(sg::Add<i32>,
              NODE_INPUTS(
                  &sg::Add<i32>::m_Value1,
                  &sg::Add<i32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Add<i32>::m_Out));

DESCRIBE_NODE(sg::Subtract<i32>,
              NODE_INPUTS(
                  &sg::Subtract<i32>::m_Value1,
                  &sg::Subtract<i32>::m_Value2),
              NODE_OUTPUTS(
                  &sg::Subtract<i32>::m_Out));

DESCRIBE_NODE(sg::Multiply<i32>,
              NODE_INPUTS(
                  &sg::Multiply<i32>::m_Value,
                  &sg::Multiply<i32>::m_Multiplier),
              NODE_OUTPUTS(
                  &sg::Multiply<i32>::m_Out));

//==============================================================================
// Envelope Nodes Descriptions
//==============================================================================
DESCRIBE_NODE(sg::ADEnvelope,
              NODE_INPUTS(
                  &sg::ADEnvelope::m_AttackTime,
                  &sg::ADEnvelope::m_DecayTime,
                  &sg::ADEnvelope::m_AttackCurve,
                  &sg::ADEnvelope::m_DecayCurve,
                  &sg::ADEnvelope::m_Looping),
              NODE_OUTPUTS(
                  &sg::ADEnvelope::m_OutEnvelope,
                  &sg::ADEnvelope::m_OnTrigger,
                  &sg::ADEnvelope::m_OnComplete));

DESCRIBE_NODE(sg::ADSREnvelope,
              NODE_INPUTS(
                  &sg::ADSREnvelope::m_AttackTime,
                  &sg::ADSREnvelope::m_DecayTime,
                  &sg::ADSREnvelope::m_SustainLevel,
                  &sg::ADSREnvelope::m_ReleaseTime,
                  &sg::ADSREnvelope::m_AttackCurve,
                  &sg::ADSREnvelope::m_DecayCurve,
                  &sg::ADSREnvelope::m_ReleaseCurve),
              NODE_OUTPUTS(
                  &sg::ADSREnvelope::m_OutEnvelope,
                  &sg::ADSREnvelope::m_OnTrigger,
                  &sg::ADSREnvelope::m_OnRelease,
                  &sg::ADSREnvelope::m_OnComplete));

//==============================================================================
// Trigger Nodes Descriptions
//==============================================================================
DESCRIBE_NODE(sg::RepeatTrigger,
              NODE_INPUTS(
                  &sg::RepeatTrigger::m_Period),
              NODE_OUTPUTS(
                  &sg::RepeatTrigger::m_OutTrigger));

DESCRIBE_NODE(sg::TriggerCounter,
              NODE_INPUTS(
                  &sg::TriggerCounter::m_StartValue,
                  &sg::TriggerCounter::m_StepSize,
                  &sg::TriggerCounter::m_ResetCount),
              NODE_OUTPUTS(
                  &sg::TriggerCounter::m_OutCount,
                  &sg::TriggerCounter::m_OutValue,
                  &sg::TriggerCounter::m_OnTrigger,
                  &sg::TriggerCounter::m_OnReset));

DESCRIBE_NODE(sg::DelayedTrigger,
              NODE_INPUTS(
                  &sg::DelayedTrigger::m_DelayTime),
              NODE_OUTPUTS(
                  &sg::DelayedTrigger::m_OutDelayedTrigger,
                  &sg::DelayedTrigger::m_OnReset));

//==============================================================================
// Array Nodes Descriptions
//==============================================================================
DESCRIBE_NODE(sg::GetRandom<f32>,
              NODE_INPUTS(
                  &sg::GetRandom<f32>::m_Array,
                  &sg::GetRandom<f32>::m_Min,
                  &sg::GetRandom<f32>::m_Max,
                  &sg::GetRandom<f32>::m_Seed),
              NODE_OUTPUTS(
                  &sg::GetRandom<f32>::m_OnNext,
                  &sg::GetRandom<f32>::m_OnReset,
                  &sg::GetRandom<f32>::m_OutElement));

DESCRIBE_NODE(sg::GetRandom<i32>,
              NODE_INPUTS(
                  &sg::GetRandom<i32>::m_Array,
                  &sg::GetRandom<i32>::m_Min,
                  &sg::GetRandom<i32>::m_Max,
                  &sg::GetRandom<i32>::m_Seed),
              NODE_OUTPUTS(
                  &sg::GetRandom<i32>::m_OnNext,
                  &sg::GetRandom<i32>::m_OnReset,
                  &sg::GetRandom<i32>::m_OutElement));

DESCRIBE_NODE(sg::Get<f32>,
              NODE_INPUTS(
                  &sg::Get<f32>::m_Array,
                  &sg::Get<f32>::m_Index),
              NODE_OUTPUTS(
                  &sg::Get<f32>::m_OnTrigger,
                  &sg::Get<f32>::m_OutElement));

DESCRIBE_NODE(sg::Get<i32>,
              NODE_INPUTS(
                  &sg::Get<i32>::m_Array,
                  &sg::Get<i32>::m_Index),
              NODE_OUTPUTS(
                  &sg::Get<i32>::m_OnTrigger,
                  &sg::Get<i32>::m_OutElement));

DESCRIBE_NODE(sg::Random<f32>,
              NODE_INPUTS(
                  &sg::Random<f32>::m_Min,
                  &sg::Random<f32>::m_Max,
                  &sg::Random<f32>::m_Seed),
              NODE_OUTPUTS(
                  &sg::Random<f32>::m_OnNext,
                  &sg::Random<f32>::m_OnReset,
                  &sg::Random<f32>::m_OutValue));

DESCRIBE_NODE(sg::Random<i32>,
              NODE_INPUTS(
                  &sg::Random<i32>::m_Min,
                  &sg::Random<i32>::m_Max,
                  &sg::Random<i32>::m_Seed),
              NODE_OUTPUTS(
                  &sg::Random<i32>::m_OnNext,
                  &sg::Random<i32>::m_OnReset,
                  &sg::Random<i32>::m_OutValue));

//==============================================================================
// Music Nodes Descriptions
//==============================================================================
DESCRIBE_NODE(sg::BPMToSeconds,
              NODE_INPUTS(
                  &sg::BPMToSeconds::m_BPM),
              NODE_OUTPUTS(
                  &sg::BPMToSeconds::m_OutSeconds));

DESCRIBE_NODE(sg::NoteToFrequency<f32>,
              NODE_INPUTS(
                  &sg::NoteToFrequency<f32>::m_MIDINote),
              NODE_OUTPUTS(
                  &sg::NoteToFrequency<f32>::m_OutFrequency));

DESCRIBE_NODE(sg::NoteToFrequency<i32>,
              NODE_INPUTS(
                  &sg::NoteToFrequency<i32>::m_MIDINote),
              NODE_OUTPUTS(
                  &sg::NoteToFrequency<i32>::m_OutFrequency));

DESCRIBE_NODE(sg::FrequencyToNote,
              NODE_INPUTS(
                  &sg::FrequencyToNote::m_Frequency),
              NODE_OUTPUTS(
                  &sg::FrequencyToNote::m_OutMIDINote));
