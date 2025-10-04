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
    DESCRIBE_NODE(sg::Add<float>,
        NODE_INPUTS(
            &sg::Add<float>::m_InValue1,
            &sg::Add<float>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Add<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Subtract<float>,
        NODE_INPUTS(
            &sg::Subtract<float>::m_InValue1,
            &sg::Subtract<float>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Subtract<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Multiply<float>,
        NODE_INPUTS(
            &sg::Multiply<float>::m_InValue,
            &sg::Multiply<float>::m_InMultiplier
        ),
        NODE_OUTPUTS(
            &sg::Multiply<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Divide<float>,
        NODE_INPUTS(
            &sg::Divide<float>::m_InValue,
            &sg::Divide<float>::m_InDenominator
        ),
        NODE_OUTPUTS(
            &sg::Divide<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Min<float>,
        NODE_INPUTS(
            &sg::Min<float>::m_InValue1,
            &sg::Min<float>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Min<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Max<float>,
        NODE_INPUTS(
            &sg::Max<float>::m_InValue1,
            &sg::Max<float>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Max<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Clamp<float>,
        NODE_INPUTS(
            &sg::Clamp<float>::m_InValue,
            &sg::Clamp<float>::m_InMinValue,
            &sg::Clamp<float>::m_InMaxValue
        ),
        NODE_OUTPUTS(
            &sg::Clamp<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::MapRange<float>,
        NODE_INPUTS(
            &sg::MapRange<float>::m_InValue,
            &sg::MapRange<float>::m_InFromMin,
            &sg::MapRange<float>::m_InFromMax,
            &sg::MapRange<float>::m_InToMin,
            &sg::MapRange<float>::m_InToMax
        ),
        NODE_OUTPUTS(
            &sg::MapRange<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Power<float>,
        NODE_INPUTS(
            &sg::Power<float>::m_InBase,
            &sg::Power<float>::m_InExponent
        ),
        NODE_OUTPUTS(
            &sg::Power<float>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Abs<float>,
        NODE_INPUTS(
            &sg::Abs<float>::m_InValue
        ),
        NODE_OUTPUTS(
            &sg::Abs<float>::m_OutOut
        )
    );

    // Integer specializations
    DESCRIBE_NODE(sg::Add<int>,
        NODE_INPUTS(
            &sg::Add<int>::m_InValue1,
            &sg::Add<int>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Add<int>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Subtract<int>,
        NODE_INPUTS(
            &sg::Subtract<int>::m_InValue1,
            &sg::Subtract<int>::m_InValue2
        ),
        NODE_OUTPUTS(
            &sg::Subtract<int>::m_OutOut
        )
    );

    DESCRIBE_NODE(sg::Multiply<int>,
        NODE_INPUTS(
            &sg::Multiply<int>::m_InValue,
            &sg::Multiply<int>::m_InMultiplier
        ),
        NODE_OUTPUTS(
            &sg::Multiply<int>::m_OutOut
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
    DESCRIBE_NODE(sg::GetRandom<float>,
        NODE_INPUTS(
            &sg::GetRandom<float>::m_InArray,
            &sg::GetRandom<float>::m_InMin,
            &sg::GetRandom<float>::m_InMax,
            &sg::GetRandom<float>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::GetRandom<float>::m_OutOnNext,
            &sg::GetRandom<float>::m_OutOnReset,
            &sg::GetRandom<float>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::GetRandom<int>,
        NODE_INPUTS(
            &sg::GetRandom<int>::m_InArray,
            &sg::GetRandom<int>::m_InMin,
            &sg::GetRandom<int>::m_InMax,
            &sg::GetRandom<int>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::GetRandom<int>::m_OutOnNext,
            &sg::GetRandom<int>::m_OutOnReset,
            &sg::GetRandom<int>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Get<float>,
        NODE_INPUTS(
            &sg::Get<float>::m_InArray,
            &sg::Get<float>::m_InIndex
        ),
        NODE_OUTPUTS(
            &sg::Get<float>::m_OutOnTrigger,
            &sg::Get<float>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Get<int>,
        NODE_INPUTS(
            &sg::Get<int>::m_InArray,
            &sg::Get<int>::m_InIndex
        ),
        NODE_OUTPUTS(
            &sg::Get<int>::m_OutOnTrigger,
            &sg::Get<int>::m_OutElement
        )
    );

    DESCRIBE_NODE(sg::Random<float>,
        NODE_INPUTS(
            &sg::Random<float>::m_InMin,
            &sg::Random<float>::m_InMax,
            &sg::Random<float>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::Random<float>::m_OutOnNext,
            &sg::Random<float>::m_OutOnReset,
            &sg::Random<float>::m_OutValue
        )
    );

    DESCRIBE_NODE(sg::Random<int>,
        NODE_INPUTS(
            &sg::Random<int>::m_InMin,
            &sg::Random<int>::m_InMax,
            &sg::Random<int>::m_InSeed
        ),
        NODE_OUTPUTS(
            &sg::Random<int>::m_OutOnNext,
            &sg::Random<int>::m_OutOnReset,
            &sg::Random<int>::m_OutValue
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

    DESCRIBE_NODE(sg::NoteToFrequency<float>,
        NODE_INPUTS(
            &sg::NoteToFrequency<float>::m_InMIDINote
        ),
        NODE_OUTPUTS(
            &sg::NoteToFrequency<float>::m_OutFrequency
        )
    );

    DESCRIBE_NODE(sg::NoteToFrequency<int>,
        NODE_INPUTS(
            &sg::NoteToFrequency<int>::m_InMIDINote
        ),
        NODE_OUTPUTS(
            &sg::NoteToFrequency<int>::m_OutFrequency
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
