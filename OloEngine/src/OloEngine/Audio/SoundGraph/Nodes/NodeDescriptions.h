#pragma once

#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "WavePlayer.h"
#include "GeneratorNodes.h"
#include "MathNodes.h"
#include "EnvelopeNodes.h"
#include "TriggerNodes.h"
#include "ArrayNodes.h"
#include "MusicNodes.h"

// Node descriptions for all implemented sound graph nodes
// This enables automatic endpoint registration and initialization

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    // WavePlayer Node Description
    //==============================================================================
    DESCRIBE_NODE(WavePlayer,
                  NODE_INPUTS(
                      &WavePlayer::in_WaveAsset,
                      &WavePlayer::in_StartTime,
                      &WavePlayer::in_Loop,
                      &WavePlayer::in_NumberOfLoops),
                  NODE_OUTPUTS(
                      &WavePlayer::out_OutLeft,
                      &WavePlayer::out_OutRight,
                      &WavePlayer::out_OnPlay,
                      &WavePlayer::out_OnStop,
                      &WavePlayer::out_OnFinished));

    //==============================================================================
    // Generator Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(SineOscillator,
                  NODE_INPUTS(
                      &SineOscillator::in_Frequency,
                      &SineOscillator::in_Amplitude,
                      &SineOscillator::in_Phase),
                  NODE_OUTPUTS(
                      &SineOscillator::out_Value));

    DESCRIBE_NODE(SquareOscillator,
                  NODE_INPUTS(
                      &SquareOscillator::in_Frequency,
                      &SquareOscillator::in_Amplitude,
                      &SquareOscillator::in_Phase,
                      &SquareOscillator::in_PulseWidth),
                  NODE_OUTPUTS(
                      &SquareOscillator::out_Value));

    DESCRIBE_NODE(SawtoothOscillator,
                  NODE_INPUTS(
                      &SawtoothOscillator::in_Frequency,
                      &SawtoothOscillator::in_Amplitude,
                      &SawtoothOscillator::in_Phase),
                  NODE_OUTPUTS(
                      &SawtoothOscillator::out_Value));

    DESCRIBE_NODE(TriangleOscillator,
                  NODE_INPUTS(
                      &TriangleOscillator::in_Frequency,
                      &TriangleOscillator::in_Amplitude,
                      &TriangleOscillator::in_Phase),
                  NODE_OUTPUTS(
                      &TriangleOscillator::out_Value));

    DESCRIBE_NODE(Noise,
                  NODE_INPUTS(
                      &Noise::in_Seed,
                      &Noise::in_Type,
                      &Noise::in_Amplitude),
                  NODE_OUTPUTS(
                      &Noise::out_Value));

    //==============================================================================
    // Math Nodes Descriptions - Template Specializations
    //==============================================================================

    // Float specializations
    DESCRIBE_NODE(Add<f32>,
                  NODE_INPUTS(
                      &Add<f32>::in_Value1,
                      &Add<f32>::in_Value2),
                  NODE_OUTPUTS(
                      &Add<f32>::out_Out));

    DESCRIBE_NODE(Subtract<f32>,
                  NODE_INPUTS(
                      &Subtract<f32>::in_Value1,
                      &Subtract<f32>::in_Value2),
                  NODE_OUTPUTS(
                      &Subtract<f32>::out_Out));

    DESCRIBE_NODE(Multiply<f32>,
                  NODE_INPUTS(
                      &Multiply<f32>::in_Value,
                      &Multiply<f32>::in_Multiplier),
                  NODE_OUTPUTS(
                      &Multiply<f32>::out_Out));

    DESCRIBE_NODE(Divide<f32>,
                  NODE_INPUTS(
                      &Divide<f32>::in_Value,
                      &Divide<f32>::in_Denominator),
                  NODE_OUTPUTS(
                      &Divide<f32>::out_Out));

    DESCRIBE_NODE(Min<f32>,
                  NODE_INPUTS(
                      &Min<f32>::in_Value1,
                      &Min<f32>::in_Value2),
                  NODE_OUTPUTS(
                      &Min<f32>::out_Out));

    DESCRIBE_NODE(Max<f32>,
                  NODE_INPUTS(
                      &Max<f32>::in_Value1,
                      &Max<f32>::in_Value2),
                  NODE_OUTPUTS(
                      &Max<f32>::out_Out));

    DESCRIBE_NODE(Clamp<f32>,
                  NODE_INPUTS(
                      &Clamp<f32>::in_Value,
                      &Clamp<f32>::in_MinValue,
                      &Clamp<f32>::in_MaxValue),
                  NODE_OUTPUTS(
                      &Clamp<f32>::out_Out));

    DESCRIBE_NODE(MapRange<f32>,
                  NODE_INPUTS(
                      &MapRange<f32>::in_Value,
                      &MapRange<f32>::in_FromMin,
                      &MapRange<f32>::in_FromMax,
                      &MapRange<f32>::in_ToMin,
                      &MapRange<f32>::in_ToMax),
                  NODE_OUTPUTS(
                      &MapRange<f32>::out_Out));

    DESCRIBE_NODE(Power<f32>,
                  NODE_INPUTS(
                      &Power<f32>::in_Base,
                      &Power<f32>::in_Exponent),
                  NODE_OUTPUTS(
                      &Power<f32>::out_Out));

    DESCRIBE_NODE(Abs<f32>,
                  NODE_INPUTS(
                      &Abs<f32>::in_Value),
                  NODE_OUTPUTS(
                      &Abs<f32>::out_Out));

    // Integer specializations
    DESCRIBE_NODE(Add<i32>,
                  NODE_INPUTS(
                      &Add<i32>::in_Value1,
                      &Add<i32>::in_Value2),
                  NODE_OUTPUTS(
                      &Add<i32>::out_Out));

    DESCRIBE_NODE(Subtract<i32>,
                  NODE_INPUTS(
                      &Subtract<i32>::in_Value1,
                      &Subtract<i32>::in_Value2),
                  NODE_OUTPUTS(
                      &Subtract<i32>::out_Out));

    DESCRIBE_NODE(Multiply<i32>,
                  NODE_INPUTS(
                      &Multiply<i32>::in_Value,
                      &Multiply<i32>::in_Multiplier),
                  NODE_OUTPUTS(
                      &Multiply<i32>::out_Out));

    //==============================================================================
    // Envelope Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(ADEnvelope,
                  NODE_INPUTS(
                      &ADEnvelope::in_AttackTime,
                      &ADEnvelope::in_DecayTime,
                      &ADEnvelope::in_AttackCurve,
                      &ADEnvelope::in_DecayCurve,
                      &ADEnvelope::in_Looping),
                  NODE_OUTPUTS(
                      &ADEnvelope::out_OutEnvelope,
                      &ADEnvelope::out_OnTrigger,
                      &ADEnvelope::out_OnComplete));

    DESCRIBE_NODE(ADSREnvelope,
                  NODE_INPUTS(
                      &ADSREnvelope::in_AttackTime,
                      &ADSREnvelope::in_DecayTime,
                      &ADSREnvelope::in_SustainLevel,
                      &ADSREnvelope::in_ReleaseTime,
                      &ADSREnvelope::in_AttackCurve,
                      &ADSREnvelope::in_DecayCurve,
                      &ADSREnvelope::in_ReleaseCurve),
                  NODE_OUTPUTS(
                      &ADSREnvelope::out_OutEnvelope,
                      &ADSREnvelope::out_OnTrigger,
                      &ADSREnvelope::out_OnRelease,
                      &ADSREnvelope::out_OnComplete));

    //==============================================================================
    // Trigger Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(RepeatTrigger,
                  NODE_INPUTS(
                      &RepeatTrigger::in_Period),
                  NODE_OUTPUTS(
                      &RepeatTrigger::out_Trigger));

    DESCRIBE_NODE(TriggerCounter,
                  NODE_INPUTS(
                      &TriggerCounter::in_StartValue,
                      &TriggerCounter::in_StepSize,
                      &TriggerCounter::in_ResetCount),
                  NODE_OUTPUTS(
                      &TriggerCounter::out_Count,
                      &TriggerCounter::out_Value,
                      &TriggerCounter::out_OnTrigger,
                      &TriggerCounter::out_OnReset));

    DESCRIBE_NODE(DelayedTrigger,
                  NODE_INPUTS(
                      &DelayedTrigger::in_DelayTime),
                  NODE_OUTPUTS(
                      &DelayedTrigger::out_DelayedTrigger,
                      &DelayedTrigger::out_OnReset));

    //==============================================================================
    // Array Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(GetRandom<f32>,
                  NODE_INPUTS(
                      &GetRandom<f32>::in_Array,
                      &GetRandom<f32>::in_Min,
                      &GetRandom<f32>::in_Max,
                      &GetRandom<f32>::in_Seed),
                  NODE_OUTPUTS(
                      &GetRandom<f32>::out_OnNext,
                      &GetRandom<f32>::out_OnReset,
                      &GetRandom<f32>::out_Element));

    DESCRIBE_NODE(GetRandom<i32>,
                  NODE_INPUTS(
                      &GetRandom<i32>::in_Array,
                      &GetRandom<i32>::in_Min,
                      &GetRandom<i32>::in_Max,
                      &GetRandom<i32>::in_Seed),
                  NODE_OUTPUTS(
                      &GetRandom<i32>::out_OnNext,
                      &GetRandom<i32>::out_OnReset,
                      &GetRandom<i32>::out_Element));

    DESCRIBE_NODE(Get<f32>,
                  NODE_INPUTS(
                      &Get<f32>::in_Array,
                      &Get<f32>::in_Index),
                  NODE_OUTPUTS(
                      &Get<f32>::out_OnTrigger,
                      &Get<f32>::out_Element));

    DESCRIBE_NODE(Get<i32>,
                  NODE_INPUTS(
                      &Get<i32>::in_Array,
                      &Get<i32>::in_Index),
                  NODE_OUTPUTS(
                      &Get<i32>::out_OnTrigger,
                      &Get<i32>::out_Element));

    DESCRIBE_NODE(Random<f32>,
                  NODE_INPUTS(
                      &Random<f32>::in_Min,
                      &Random<f32>::in_Max,
                      &Random<f32>::in_Seed),
                  NODE_OUTPUTS(
                      &Random<f32>::out_OnNext,
                      &Random<f32>::out_OnReset,
                      &Random<f32>::out_Value));

    DESCRIBE_NODE(Random<i32>,
                  NODE_INPUTS(
                      &Random<i32>::in_Min,
                      &Random<i32>::in_Max,
                      &Random<i32>::in_Seed),
                  NODE_OUTPUTS(
                      &Random<i32>::out_OnNext,
                      &Random<i32>::out_OnReset,
                      &Random<i32>::out_Value));

    //==============================================================================
    // Music Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(BPMToSeconds,
                  NODE_INPUTS(
                      &BPMToSeconds::in_BPM),
                  NODE_OUTPUTS(
                      &BPMToSeconds::out_Seconds));

    DESCRIBE_NODE(NoteToFrequency<f32>,
                  NODE_INPUTS(
                      &NoteToFrequency<f32>::in_MIDINote),
                  NODE_OUTPUTS(
                      &NoteToFrequency<f32>::out_Frequency));

    DESCRIBE_NODE(NoteToFrequency<i32>,
                  NODE_INPUTS(
                      &NoteToFrequency<i32>::in_MIDINote),
                  NODE_OUTPUTS(
                      &NoteToFrequency<i32>::out_Frequency));

    DESCRIBE_NODE(FrequencyToNote,
                  NODE_INPUTS(
                      &FrequencyToNote::in_Frequency),
                  NODE_OUTPUTS(
                      &FrequencyToNote::out_MIDINote));

} // namespace OloEngine::Audio::SoundGraph
