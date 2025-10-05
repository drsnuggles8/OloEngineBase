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

namespace sg = OloEngine::Audio::SoundGraph;

//==============================================================================
// WavePlayer Node Description
//==============================================================================
DESCRIBE_NODE(sg::WavePlayer,
        NODE_INPUTS(
            &sg::WavePlayer::m_InWaveAsset,
            &sg::WavePlayer::m_InStartTime,
            &sg::WavePlayer::m_InLoop,
            &sg::WavePlayer::m_InNumberOfLoops
        ),
        NODE_OUTPUTS(
            &sg::WavePlayer::m_OutOutLeft,
            &sg::WavePlayer::m_OutOutRight,
            &sg::WavePlayer::m_OutOnPlay,
            &sg::WavePlayer::m_OutOnStop,
            &sg::WavePlayer::m_OutOnFinished
        )
    );

    //==============================================================================
    // Generator Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(sg::SineOscillator,
        NODE_INPUTS(
            &sg::SineOscillator::m_InFrequency,
            &sg::SineOscillator::m_InAmplitude,
            &sg::SineOscillator::m_InPhase
        ),
        NODE_OUTPUTS(
            &sg::SineOscillator::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::SquareOscillator,
        NODE_INPUTS(
            &sg::SquareOscillator::m_InFrequency,
            &sg::SquareOscillator::m_InAmplitude,
            &sg::SquareOscillator::m_InPhase,
            &sg::SquareOscillator::m_InPulseWidth
        ),
        NODE_OUTPUTS(
            &sg::SquareOscillator::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::SawtoothOscillator,
        NODE_INPUTS(
            &sg::SawtoothOscillator::m_InFrequency,
            &sg::SawtoothOscillator::m_InAmplitude,
            &sg::SawtoothOscillator::m_InPhase
        ),
        NODE_OUTPUTS(
            &sg::SawtoothOscillator::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::TriangleOscillator,
        NODE_INPUTS(
            &sg::TriangleOscillator::m_InFrequency,
            &sg::TriangleOscillator::m_InAmplitude,
            &sg::TriangleOscillator::m_InPhase
        ),
        NODE_OUTPUTS(
            &sg::TriangleOscillator::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::Noise,
        NODE_INPUTS(
            &sg::Noise::m_InSeed,
            &sg::Noise::m_InType,
            &sg::Noise::m_InAmplitude
        ),
        NODE_OUTPUTS(
            &sg::Noise::m_OutValue
        )
    );

    //==============================================================================
    // Math Nodes Descriptions - Template Specializations
    //==============================================================================
    
    // Float specializations
    DESCRIBE_NODE(sg::Add<f32>,
        NODE_INPUTS(
            &sg::Add<f32>::m_InValue1,
            &sg::Add<f32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Add<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Subtract<f32>,
        NODE_INPUTS(
            &sg::Subtract<f32>::m_InValue1,
            &sg::Subtract<f32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Subtract<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Multiply<f32>,
        NODE_INPUTS(
            &sg::Multiply<f32>::m_InValue,
            &sg::Multiply<f32>::m_InMultiplier
        ),
        NODE_OUTPUTS(
            &sg::Multiply<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Divide<f32>,
        NODE_INPUTS(
            &sg::Divide<f32>::m_InValue,
            &sg::Divide<f32>::m_InDenominator
        ),
        NODE_OUTPUTS(
            &sg::Divide<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Min<f32>,
        NODE_INPUTS(
            &sg::Min<f32>::m_InValue1,
            &sg::Min<f32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Min<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Max<f32>,
        NODE_INPUTS(
            &sg::Max<f32>::m_InValue1,
            &sg::Max<f32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Max<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Clamp<f32>,
        NODE_INPUTS(
            &sg::Clamp<f32>::m_InValue,
            &sg::Clamp<f32>::m_InMinValue,
            &sg::Clamp<f32>::m_InMaxValue
        ),
        NODE_OUTPUTS(
            &sg::Clamp<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::MapRange<f32>,
        NODE_INPUTS(
            &sg::MapRange<f32>::m_InValue,
            &sg::MapRange<f32>::m_InFromMin,
            &sg::MapRange<f32>::m_InFromMax,
            &sg::MapRange<f32>::m_InToMin,
            &sg::MapRange<f32>::m_InToMax
        ),
        NODE_OUTPUTS(
            &sg::MapRange<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Power<f32>,
        NODE_INPUTS(
            &sg::Power<f32>::m_InBase,
            &sg::Power<f32>::m_InExponent
        ),
        NODE_OUTPUTS(
            &sg::Power<f32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Abs<f32>,
        NODE_INPUTS(
            &sg::Abs<f32>::m_InValue
        ),
        NODE_OUTPUTS(
            &sg::Abs<f32>::m_OutOut
        )
    );

    // Integer specializations
    DESCRIBE_NODE(sg::Add<i32>,
        NODE_INPUTS(
            &sg::Add<i32>::m_InValue1,
            &sg::Add<i32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Add<i32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Subtract<i32>,
        NODE_INPUTS(
            &sg::Subtract<i32>::m_InValue1,
            &sg::Subtract<i32>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Subtract<i32>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Multiply<i32>,
        NODE_INPUTS(
            &sg::Multiply<i32>::m_InValue,
            &sg::Multiply<i32>::m_InMultiplier
        ),
        NODE_OUTPUTS(
            &sg::Multiply<i32>::m_OutOut
        )
    );

    //==============================================================================
    // Envelope Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(sg::ADEnvelope,
        NODE_INPUTS(
            &sg::ADEnvelope::m_InAttackTime,
            &sg::ADEnvelope::m_InDecayTime,
            &sg::ADEnvelope::m_InAttackCurve,
            &sg::ADEnvelope::m_InDecayCurve,
            &sg::ADEnvelope::m_InLooping
        ),
        NODE_OUTPUTS(
            &sg::ADEnvelope::m_OutOutEnvelope,
            &sg::ADEnvelope::m_OutOnTrigger,
            &sg::ADEnvelope::m_OutOnComplete
        )
    );

    DESCRIBE_NODE(sg::ADSREnvelope,
        NODE_INPUTS(
            &sg::ADSREnvelope::m_InAttackTime,
            &sg::ADSREnvelope::m_InDecayTime,
            &sg::ADSREnvelope::m_InSustainLevel,
            &sg::ADSREnvelope::m_InReleaseTime,
            &sg::ADSREnvelope::m_InAttackCurve,
            &sg::ADSREnvelope::m_InDecayCurve,
            &sg::ADSREnvelope::m_InReleaseCurve
        ),
        NODE_OUTPUTS(
            &sg::ADSREnvelope::m_OutOutEnvelope,
            &sg::ADSREnvelope::m_OutOnTrigger,
            &sg::ADSREnvelope::m_OutOnRelease,
            &sg::ADSREnvelope::m_OutOnComplete
        )
    );

    //==============================================================================
    // Trigger Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(sg::RepeatTrigger,
        NODE_INPUTS(
            &sg::RepeatTrigger::m_InPeriod
        ),
        NODE_OUTPUTS(
            &sg::RepeatTrigger::m_OutTrigger
        )
    );

    DESCRIBE_NODE(sg::TriggerCounter,
        NODE_INPUTS(
            &sg::TriggerCounter::m_InStartValue,
            &sg::TriggerCounter::m_InStepSize,
            &sg::TriggerCounter::m_InResetCount
        ),
        NODE_OUTPUTS(
            &sg::TriggerCounter::m_OutCount,
            &sg::TriggerCounter::m_OutValue,
            &sg::TriggerCounter::m_OutOnTrigger,
            &sg::TriggerCounter::m_OutOnReset
        )
    );

    DESCRIBE_NODE(sg::DelayedTrigger,
        NODE_INPUTS(
            &sg::DelayedTrigger::m_InDelayTime
        ),
        NODE_OUTPUTS(
            &sg::DelayedTrigger::m_OutDelayedTrigger,
            &sg::DelayedTrigger::m_OutOnReset
        )
    );

    //==============================================================================
    // Array Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(sg::GetRandom<f32>,
        NODE_INPUTS(
            &sg::GetRandom<f32>::m_InArray,
            &sg::GetRandom<f32>::m_InMin,
            &sg::GetRandom<f32>::m_InMax,
            &sg::GetRandom<f32>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::GetRandom<f32>::m_OutOnNext,
            &sg::GetRandom<f32>::m_OutOnReset,
            &sg::GetRandom<f32>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::GetRandom<i32>,
        NODE_INPUTS(
            &sg::GetRandom<i32>::m_InArray,
            &sg::GetRandom<i32>::m_InMin,
            &sg::GetRandom<i32>::m_InMax,
            &sg::GetRandom<i32>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::GetRandom<i32>::m_OutOnNext,
            &sg::GetRandom<i32>::m_OutOnReset,
            &sg::GetRandom<i32>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Get<f32>,
        NODE_INPUTS(
            &sg::Get<f32>::m_InArray,
            &sg::Get<f32>::m_InIndex
        ),
        NODE_OUTPUTS(
            &sg::Get<f32>::m_OutOnTrigger,
            &sg::Get<f32>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Get<i32>,
        NODE_INPUTS(
            &sg::Get<i32>::m_InArray,
            &sg::Get<i32>::m_InIndex
        ),
        NODE_OUTPUTS(
            &sg::Get<i32>::m_OutOnTrigger,
            &sg::Get<i32>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Random<f32>,
        NODE_INPUTS(
            &sg::Random<f32>::m_InMin,
            &sg::Random<f32>::m_InMax,
            &sg::Random<f32>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::Random<f32>::m_OutOnNext,
            &sg::Random<f32>::m_OutOnReset,
            &sg::Random<f32>::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::Random<i32>,
        NODE_INPUTS(
            &sg::Random<i32>::m_InMin,
            &sg::Random<i32>::m_InMax,
            &sg::Random<i32>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::Random<i32>::m_OutOnNext,
            &sg::Random<i32>::m_OutOnReset,
            &sg::Random<i32>::m_OutValue
        )
    );

    //==============================================================================
    // Music Nodes Descriptions
    //==============================================================================
    DESCRIBE_NODE(sg::BPMToSeconds,
        NODE_INPUTS(
            &sg::BPMToSeconds::m_InBPM
        ),
        NODE_OUTPUTS(
            &sg::BPMToSeconds::m_OutSeconds
        )
    );

    DESCRIBE_NODE(sg::NoteToFrequency<f32>,
        NODE_INPUTS(
            &sg::NoteToFrequency<f32>::m_InMIDINote
        ),
        NODE_OUTPUTS(
            &sg::NoteToFrequency<f32>::m_OutFrequency
        )
    );

    DESCRIBE_NODE(sg::NoteToFrequency<i32>,
        NODE_INPUTS(
            &sg::NoteToFrequency<i32>::m_InMIDINote
        ),
        NODE_OUTPUTS(
            &sg::NoteToFrequency<i32>::m_OutFrequency
        )
    );

    DESCRIBE_NODE(sg::FrequencyToNote,
        NODE_INPUTS(
            &sg::FrequencyToNote::m_InFrequency
        ),
        NODE_OUTPUTS(
            &sg::FrequencyToNote::m_OutMIDINote
        )
    );
