#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GeneratorNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MathNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/EnvelopeNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriggerNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ArrayNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MusicNodes.h"

// Node descriptions for all implemented sound graph nodes
// This enables automatic endpoint registration and initialization

// NOTE: DESCRIBE_NODE macros must be at global scope (outside any namespace)
// because they expand to template specializations in OloEngine::Core::Reflection namespace

using namespace OloEngine::Audio::SoundGraph;

//==============================================================================
// WavePlayer Node Description
//==============================================================================
DESCRIBE_NODE(WavePlayer,
        NODE_INPUTS(
            &WavePlayer::in_WaveAsset,
            &WavePlayer::in_StartTime,
            &WavePlayer::in_Loop,
            &WavePlayer::in_NumberOfLoops
        ),
        NODE_OUTPUTS(
            &WavePlayer::out_OutLeft,
            &WavePlayer::out_OutRight,
            &WavePlayer::out_OnPlay,
            &WavePlayer::out_OnStop,
            &WavePlayer::out_OnFinished
        )
    );

    //==============================================================================
    // Generator Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(SineOscillator,
        NODE_INPUTS(
            &SineOscillator::in_Frequency,
            &SineOscillator::in_Amplitude,
            &SineOscillator::in_Phase
        ),
        NODE_OUTPUTS(
            &SineOscillator::out_Value
        )
    );

    DESCRIBE_NODE(SquareOscillator,
        NODE_INPUTS(
            &SquareOscillator::in_Frequency,
            &SquareOscillator::in_Amplitude,
            &SquareOscillator::in_Phase,
            &SquareOscillator::in_PulseWidth
        ),
        NODE_OUTPUTS(
            &SquareOscillator::out_Value
        )
    );

    DESCRIBE_NODE(SawtoothOscillator,
        NODE_INPUTS(
            &SawtoothOscillator::in_Frequency,
            &SawtoothOscillator::in_Amplitude,
            &SawtoothOscillator::in_Phase
        ),
        NODE_OUTPUTS(
            &SawtoothOscillator::out_Value
        )
    );

    DESCRIBE_NODE(TriangleOscillator,
        NODE_INPUTS(
            &TriangleOscillator::in_Frequency,
            &TriangleOscillator::in_Amplitude,
            &TriangleOscillator::in_Phase
        ),
        NODE_OUTPUTS(
            &TriangleOscillator::out_Value
        )
    );

    DESCRIBE_NODE(Noise,
        NODE_INPUTS(
            &Noise::in_Seed,
            &Noise::in_Type,
            &Noise::in_Amplitude
        ),
        NODE_OUTPUTS(
            &Noise::out_Value
        )
    );

    //==============================================================================
    // Math Nodes Descriptions - Template Specializations
    //==============================================================================
    
    // Float specializations
    DESCRIBE_NODE(Add<float>,
        NODE_INPUTS(
            &Add<float>::in_Value1,
            &Add<float>::in_Value2
        ),
        NODE_OUTPUTS(
            &Add<float>::out_Out
        )
    );

    DESCRIBE_NODE(Subtract<float>,
        NODE_INPUTS(
            &Subtract<float>::in_Value1,
            &Subtract<float>::in_Value2
        ),
        NODE_OUTPUTS(
            &Subtract<float>::out_Out
        )
    );

    DESCRIBE_NODE(Multiply<float>,
        NODE_INPUTS(
            &Multiply<float>::in_Value,
            &Multiply<float>::in_Multiplier
        ),
        NODE_OUTPUTS(
            &Multiply<float>::out_Out
        )
    );

    DESCRIBE_NODE(Divide<float>,
        NODE_INPUTS(
            &Divide<float>::in_Value,
            &Divide<float>::in_Denominator
        ),
        NODE_OUTPUTS(
            &Divide<float>::out_Out
        )
    );

    DESCRIBE_NODE(Min<float>,
        NODE_INPUTS(
            &Min<float>::in_Value1,
            &Min<float>::in_Value2
        ),
        NODE_OUTPUTS(
            &Min<float>::out_Out
        )
    );

    DESCRIBE_NODE(Max<float>,
        NODE_INPUTS(
            &Max<float>::in_Value1,
            &Max<float>::in_Value2
        ),
        NODE_OUTPUTS(
            &Max<float>::out_Out
        )
    );

    DESCRIBE_NODE(Clamp<float>,
        NODE_INPUTS(
            &Clamp<float>::in_Value,
            &Clamp<float>::in_MinValue,
            &Clamp<float>::in_MaxValue
        ),
        NODE_OUTPUTS(
            &Clamp<float>::out_Out
        )
    );

    DESCRIBE_NODE(MapRange<float>,
        NODE_INPUTS(
            &MapRange<float>::in_Value,
            &MapRange<float>::in_FromMin,
            &MapRange<float>::in_FromMax,
            &MapRange<float>::in_ToMin,
            &MapRange<float>::in_ToMax
        ),
        NODE_OUTPUTS(
            &MapRange<float>::out_Out
        )
    );

    DESCRIBE_NODE(Power<float>,
        NODE_INPUTS(
            &Power<float>::in_Base,
            &Power<float>::in_Exponent
        ),
        NODE_OUTPUTS(
            &Power<float>::out_Out
        )
    );

    DESCRIBE_NODE(Abs<float>,
        NODE_INPUTS(
            &Abs<float>::in_Value
        ),
        NODE_OUTPUTS(
            &Abs<float>::out_Out
        )
    );

    // Integer specializations
    DESCRIBE_NODE(Add<int>,
        NODE_INPUTS(
            &Add<int>::in_Value1,
            &Add<int>::in_Value2
        ),
        NODE_OUTPUTS(
            &Add<int>::out_Out
        )
    );

    DESCRIBE_NODE(Subtract<int>,
        NODE_INPUTS(
            &Subtract<int>::in_Value1,
            &Subtract<int>::in_Value2
        ),
        NODE_OUTPUTS(
            &Subtract<int>::out_Out
        )
    );

    DESCRIBE_NODE(Multiply<int>,
        NODE_INPUTS(
            &Multiply<int>::in_Value,
            &Multiply<int>::in_Multiplier
        ),
        NODE_OUTPUTS(
            &Multiply<int>::out_Out
        )
    );

    //==============================================================================
    // Envelope Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(ADEnvelope,
        NODE_INPUTS(
            &ADEnvelope::in_AttackTime,
            &ADEnvelope::in_DecayTime,
            &ADEnvelope::in_AttackCurve,
            &ADEnvelope::in_DecayCurve,
            &ADEnvelope::in_Looping
        ),
        NODE_OUTPUTS(
            &ADEnvelope::out_OutEnvelope,
            &ADEnvelope::out_OnTrigger,
            &ADEnvelope::out_OnComplete
        )
    );

    DESCRIBE_NODE(ADSREnvelope,
        NODE_INPUTS(
            &ADSREnvelope::in_AttackTime,
            &ADSREnvelope::in_DecayTime,
            &ADSREnvelope::in_SustainLevel,
            &ADSREnvelope::in_ReleaseTime,
            &ADSREnvelope::in_AttackCurve,
            &ADSREnvelope::in_DecayCurve,
            &ADSREnvelope::in_ReleaseCurve
        ),
        NODE_OUTPUTS(
            &ADSREnvelope::out_OutEnvelope,
            &ADSREnvelope::out_OnTrigger,
            &ADSREnvelope::out_OnRelease,
            &ADSREnvelope::out_OnComplete
        )
    );

    //==============================================================================
    // Trigger Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(RepeatTrigger,
        NODE_INPUTS(
            &RepeatTrigger::in_Period
        ),
        NODE_OUTPUTS(
            &RepeatTrigger::out_Trigger
        )
    );

    DESCRIBE_NODE(TriggerCounter,
        NODE_INPUTS(
            &TriggerCounter::in_StartValue,
            &TriggerCounter::in_StepSize,
            &TriggerCounter::in_ResetCount
        ),
        NODE_OUTPUTS(
            &TriggerCounter::out_Count,
            &TriggerCounter::out_Value,
            &TriggerCounter::out_OnTrigger,
            &TriggerCounter::out_OnReset
        )
    );

    DESCRIBE_NODE(DelayedTrigger,
        NODE_INPUTS(
            &DelayedTrigger::in_DelayTime
        ),
        NODE_OUTPUTS(
            &DelayedTrigger::out_DelayedTrigger,
            &DelayedTrigger::out_OnReset
        )
    );

    //==============================================================================
    // Array Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(GetRandom<float>,
        NODE_INPUTS(
            &GetRandom<float>::in_Array,
            &GetRandom<float>::in_Min,
            &GetRandom<float>::in_Max,
            &GetRandom<float>::in_Seed
        ),
        NODE_OUTPUTS(
            &GetRandom<float>::out_OnNext,
            &GetRandom<float>::out_OnReset,
            &GetRandom<float>::out_Element
        )
    );

    DESCRIBE_NODE(GetRandom<int>,
        NODE_INPUTS(
            &GetRandom<int>::in_Array,
            &GetRandom<int>::in_Min,
            &GetRandom<int>::in_Max,
            &GetRandom<int>::in_Seed
        ),
        NODE_OUTPUTS(
            &GetRandom<int>::out_OnNext,
            &GetRandom<int>::out_OnReset,
            &GetRandom<int>::out_Element
        )
    );

    DESCRIBE_NODE(Get<float>,
        NODE_INPUTS(
            &Get<float>::in_Array,
            &Get<float>::in_Index
        ),
        NODE_OUTPUTS(
            &Get<float>::out_OnTrigger,
            &Get<float>::out_Element
        )
    );

    DESCRIBE_NODE(Get<int>,
        NODE_INPUTS(
            &Get<int>::in_Array,
            &Get<int>::in_Index
        ),
        NODE_OUTPUTS(
            &Get<int>::out_OnTrigger,
            &Get<int>::out_Element
        )
    );

    DESCRIBE_NODE(Random<float>,
        NODE_INPUTS(
            &Random<float>::in_Min,
            &Random<float>::in_Max,
            &Random<float>::in_Seed
        ),
        NODE_OUTPUTS(
            &Random<float>::out_OnNext,
            &Random<float>::out_OnReset,
            &Random<float>::out_Value
        )
    );

    DESCRIBE_NODE(Random<int>,
        NODE_INPUTS(
            &Random<int>::in_Min,
            &Random<int>::in_Max,
            &Random<int>::in_Seed
        ),
        NODE_OUTPUTS(
            &Random<int>::out_OnNext,
            &Random<int>::out_OnReset,
            &Random<int>::out_Value
        )
    );

    //==============================================================================
    // Music Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(BPMToSeconds,
        NODE_INPUTS(
            &BPMToSeconds::in_BPM
        ),
        NODE_OUTPUTS(
            &BPMToSeconds::out_Seconds
        )
    );

    DESCRIBE_NODE(NoteToFrequency<float>,
        NODE_INPUTS(
            &NoteToFrequency<float>::in_MIDINote
        ),
        NODE_OUTPUTS(
            &NoteToFrequency<float>::out_Frequency
        )
    );

    DESCRIBE_NODE(NoteToFrequency<int>,
        NODE_INPUTS(
            &NoteToFrequency<int>::in_MIDINote
        ),
        NODE_OUTPUTS(
            &NoteToFrequency<int>::out_Frequency
        )
    );

    DESCRIBE_NODE(FrequencyToNote,
        NODE_INPUTS(
            &FrequencyToNote::in_Frequency
        ),
        NODE_OUTPUTS(
            &FrequencyToNote::out_MIDINote
        )
    );
