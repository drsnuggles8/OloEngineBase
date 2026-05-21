#include "OloEnginePCH.h"
#include "NodeSchema.h"

#include <charconv>

namespace OloEngine::Audio::SoundGraph
{
    namespace
    {
        // First-cut hand-written schemas. Add an entry per node type the user should be
        // able to parameterize. Math/trigger nodes that are wired-only (their behavior
        // is fully determined by their inputs) don't need entries.
        const std::unordered_map<std::string, NodeSchema>& GetSchemaMap()
        {
            static const std::unordered_map<std::string, NodeSchema> s_Schemas = {
                { "SineOscillator", {
                                        { "Frequency", NodeParamKind::Float, 440.0f, 0, false, 0.0f, 20000.0f, 1.0f, "Frequency in Hz" },
                                        { "Amplitude", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Output amplitude (0..1)" },
                                        { "Phase", NodeParamKind::Float, 0.0f, 0, false, 0.0f, 6.283185f, 0.01f, "Phase offset in radians" },
                                    } },
                { "SquareOscillator", {
                                          { "Frequency", NodeParamKind::Float, 440.0f, 0, false, 0.0f, 20000.0f, 1.0f, "Frequency in Hz" },
                                          { "Amplitude", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Output amplitude (0..1)" },
                                          { "Phase", NodeParamKind::Float, 0.0f, 0, false, 0.0f, 6.283185f, 0.01f, "Phase offset in radians" },
                                          { "PulseWidth", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Duty cycle (0.5 = square)" },
                                      } },
                { "SawtoothOscillator", {
                                            { "Frequency", NodeParamKind::Float, 440.0f, 0, false, 0.0f, 20000.0f, 1.0f, "Frequency in Hz" },
                                            { "Amplitude", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Output amplitude (0..1)" },
                                            { "Phase", NodeParamKind::Float, 0.0f, 0, false, 0.0f, 6.283185f, 0.01f, "Phase offset in radians" },
                                        } },
                { "TriangleOscillator", {
                                            { "Frequency", NodeParamKind::Float, 440.0f, 0, false, 0.0f, 20000.0f, 1.0f, "Frequency in Hz" },
                                            { "Amplitude", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Output amplitude (0..1)" },
                                            { "Phase", NodeParamKind::Float, 0.0f, 0, false, 0.0f, 6.283185f, 0.01f, "Phase offset in radians" },
                                        } },
                { "Noise", {
                               { "Amplitude", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Output amplitude (0..1)" },
                           } },
                { "WavePlayer", {
                                    { "WaveAsset", NodeParamKind::AudioAsset, 0.0f, 0, false, 0, 0, 0, "Audio file to play" },
                                    { "StartTime", NodeParamKind::Float, 0.0f, 0, false, 0.0f, 600.0f, 0.01f, "Start offset (seconds)" },
                                    { "Loop", NodeParamKind::Bool, 0.0f, 0, false, 0, 0, 0, "Loop playback" },
                                    { "NumberOfLoops", NodeParamKind::Int, 0.0f, -1, false, 0, 0, 0, "-1 = infinite" },
                                } },
                { "ADEnvelope", {
                                    { "AttackTime", NodeParamKind::Float, 0.01f, 0, false, 0.0f, 10.0f, 0.001f, "Attack time (seconds)" },
                                    { "DecayTime", NodeParamKind::Float, 0.2f, 0, false, 0.0f, 10.0f, 0.001f, "Decay time (seconds)" },
                                } },
                { "ADSREnvelope", {
                                      { "AttackTime", NodeParamKind::Float, 0.01f, 0, false, 0.0f, 10.0f, 0.001f, "Attack time (seconds)" },
                                      { "DecayTime", NodeParamKind::Float, 0.2f, 0, false, 0.0f, 10.0f, 0.001f, "Decay time (seconds)" },
                                      { "SustainLevel", NodeParamKind::Float, 0.5f, 0, false, 0.0f, 1.0f, 0.01f, "Sustain level (0..1)" },
                                      { "ReleaseTime", NodeParamKind::Float, 0.3f, 0, false, 0.0f, 10.0f, 0.001f, "Release time (seconds)" },
                                  } },
                // Math nodes — Add/Subtract/Min/Max have symmetric Value1/Value2 inputs;
                // Multiply / Divide use Value + Multiplier/Denominator; Power similarly.
                // Default to 0 / 1 / 1 so a brand-new node behaves predictably.
                { "Add<float>", {
                                    { "Value1", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "First operand" },
                                    { "Value2", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Second operand" },
                                } },
                { "Subtract<float>", {
                                         { "Value1", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Minuend" },
                                         { "Value2", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Subtrahend" },
                                     } },
                { "Multiply<float>", {
                                         { "Value", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input value" },
                                         { "Multiplier", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Scalar multiplier" },
                                     } },
                { "Divide<float>", {
                                       { "Value", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Numerator" },
                                       { "Denominator", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Denominator (zero is clamped at runtime)" },
                                   } },
                { "Min<float>", {
                                    { "Value1", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "First operand" },
                                    { "Value2", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Second operand" },
                                } },
                { "Max<float>", {
                                    { "Value1", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "First operand" },
                                    { "Value2", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Second operand" },
                                } },
                { "Clamp<float>", {
                                      { "Value", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input value" },
                                      { "MinValue", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Lower bound" },
                                      { "MaxValue", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Upper bound" },
                                  } },
                { "MapRange<float>", {
                                         { "Value", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input value" },
                                         { "FromMin", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input range start" },
                                         { "FromMax", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input range end" },
                                         { "ToMin", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Output range start" },
                                         { "ToMax", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Output range end" },
                                     } },
                { "Power<float>", {
                                      { "Base", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Base" },
                                      { "Exponent", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Exponent" },
                                  } },
                { "Abs<float>", {
                                    { "Value", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Input value" },
                                } },
                // Int-typed math (Add/Subtract/Multiply) — defaults to 0/0/1.
                { "Add<int>", {
                                  { "Value1", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "First operand" },
                                  { "Value2", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "Second operand" },
                              } },
                { "Subtract<int>", {
                                       { "Value1", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "Minuend" },
                                       { "Value2", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "Subtrahend" },
                                   } },
                { "Multiply<int>", {
                                       { "Value", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "Input value" },
                                       { "Multiplier", NodeParamKind::Int, 0.0f, 1, false, 0, 0, 0, "Scalar multiplier" },
                                   } },
                // Trigger nodes — RepeatTrigger fires every Period seconds; TriggerCounter
                // counts up by StepSize starting from StartValue; DelayedTrigger waits
                // DelayTime seconds before passing the trigger through.
                { "RepeatTrigger", {
                                       { "Period", NodeParamKind::Float, 1.0f, 0, false, 0.001f, 600.0f, 0.01f, "Period in seconds (>= 0.001)" },
                                   } },
                { "TriggerCounter", {
                                        { "StartValue", NodeParamKind::Float, 0.0f, 0, false, -10000.0f, 10000.0f, 0.1f, "Initial output value" },
                                        { "StepSize", NodeParamKind::Float, 1.0f, 0, false, -10000.0f, 10000.0f, 0.01f, "Added each trigger" },
                                        { "ResetCount", NodeParamKind::Int, 0.0f, 0, false, 0, 0, 0, "Steps before reset (0 = never)" },
                                    } },
                { "DelayedTrigger", {
                                        { "DelayTime", NodeParamKind::Float, 0.1f, 0, false, 0.0f, 600.0f, 0.01f, "Delay seconds before forwarding the trigger" },
                                    } },
                // Music helpers — single Float / Int input each.
                { "BPMToSeconds", {
                                      { "BPM", NodeParamKind::Float, 120.0f, 0, false, 1.0f, 999.0f, 1.0f, "Beats per minute" },
                                  } },
                { "NoteToFrequency", {
                                         { "MIDINote", NodeParamKind::Int, 0.0f, 60, false, 0, 0, 0, "MIDI note number (0..127)" },
                                     } },
                { "FrequencyToNote", {
                                         { "Frequency", NodeParamKind::Float, 440.0f, 0, false, 0.0f, 20000.0f, 1.0f, "Frequency in Hz" },
                                     } },
            };
            return s_Schemas;
        }
    } // namespace

    const NodeSchema* GetNodeSchema(const std::string& nodeType)
    {
        const auto& map = GetSchemaMap();
        auto it = map.find(nodeType);
        if (it == map.end())
            return nullptr;
        return &it->second;
    }

    namespace
    {
        // Hand-maintained mirror of the runtime endpoint registrations from
        // NodeDescriptions.cpp + each node's ctor (events). The editor uses this so a
        // freshly-placed node draws all its pins immediately, instead of starting blank
        // and only showing pins as connections are made.
        //
        // Names follow the runtime convention exactly (see NodePinDescriptor docs in
        // NodeSchema.h). When adding a node type to NodeDescriptions.cpp, add its pins
        // here too, or the editor will fall back to connection-derived discovery for it.
        const std::unordered_map<std::string, NodePinSchema>& GetPinSchemaMap()
        {
            // Shortcuts so the table below reads as data, not type names.
            auto val = [](const char* name)
            { return NodePinDescriptor{ name, false }; };
            auto ev = [](const char* name)
            { return NodePinDescriptor{ name, true }; };

            static const std::unordered_map<std::string, NodePinSchema> s_Pins = {
                // ----- WavePlayer ---------------------------------------------------
                { "WavePlayer", {
                                    /* Inputs */ { ev("Play"), ev("Stop"), val("WaveAsset"), val("StartTime"), val("Loop"), val("NumberOfLoops") },
                                    /* Outputs */ { val("OutLeft"), val("OutRight"), ev("OnPlay"), ev("OnStop"), ev("OnFinished") },
                                } },

                // ----- Generators ---------------------------------------------------
                { "SineOscillator", { { val("Frequency"), val("Amplitude"), val("Phase") }, { val("OutValue") } } },
                { "SquareOscillator", { { val("Frequency"), val("Amplitude"), val("Phase"), val("PulseWidth") }, { val("OutValue") } } },
                { "SawtoothOscillator", { { val("Frequency"), val("Amplitude"), val("Phase") }, { val("OutValue") } } },
                { "TriangleOscillator", { { val("Frequency"), val("Amplitude"), val("Phase") }, { val("OutValue") } } },
                { "Noise", { { val("Seed"), val("Type"), val("Amplitude") }, { val("OutValue") } } },

                // ----- Math (float specializations — the editor's palette only exposes f32) ---
                { "Add<float>", { { val("Value1"), val("Value2") }, { val("Out") } } },
                { "Subtract<float>", { { val("Value1"), val("Value2") }, { val("Out") } } },
                { "Multiply<float>", { { val("Value"), val("Multiplier") }, { val("Out") } } },
                { "Divide<float>", { { val("Value"), val("Denominator") }, { val("Out") } } },
                { "Min<float>", { { val("Value1"), val("Value2") }, { val("Out") } } },
                { "Max<float>", { { val("Value1"), val("Value2") }, { val("Out") } } },
                { "Clamp<float>", { { val("Value"), val("MinValue"), val("MaxValue") }, { val("Out") } } },
                { "MapRange<float>", { { val("Value"), val("FromMin"), val("FromMax"), val("ToMin"), val("ToMax") }, { val("Out") } } },
                { "Power<float>", { { val("Base"), val("Exponent") }, { val("Out") } } },
                { "Abs<float>", { { val("Value") }, { val("Out") } } },

                // ----- Envelopes ----------------------------------------------------
                { "ADEnvelope", {
                                    /* Inputs */ { ev("s_Trigger"), val("AttackTime"), val("DecayTime"), val("AttackCurve"), val("DecayCurve"), val("Looping") },
                                    /* Outputs */ { val("OutEnvelope"), ev("OnTrigger"), ev("OnComplete") },
                                } },
                { "ADSREnvelope", {
                                      /* Inputs */ { ev("s_Trigger"), ev("s_Release"), val("AttackTime"), val("DecayTime"), val("SustainLevel"), val("ReleaseTime"), val("AttackCurve"), val("DecayCurve"), val("ReleaseCurve") },
                                      /* Outputs */ { val("OutEnvelope"), ev("OnTrigger"), ev("OnRelease"), ev("OnComplete") },
                                  } },

                // ----- Triggers -----------------------------------------------------
                { "RepeatTrigger", {
                                       /* Inputs */ { ev("s_Start"), ev("s_Stop"), val("Period") },
                                       /* Outputs */ { ev("OutTrigger") },
                                   } },
                { "TriggerCounter", {
                                        /* Inputs */ { ev("s_Trigger"), ev("s_Reset"), val("StartValue"), val("StepSize"), val("ResetCount") },
                                        /* Outputs */ { val("OutCount"), val("OutValue"), ev("OnTrigger"), ev("OnReset") },
                                    } },
                { "DelayedTrigger", {
                                        /* Inputs */ { ev("s_Trigger"), ev("s_Reset"), val("DelayTime") },
                                        /* Outputs */ { ev("OutDelayedTrigger"), ev("OnReset") },
                                    } },

                // ----- Music helpers ------------------------------------------------
                { "BPMToSeconds", { { val("BPM") }, { val("OutSeconds") } } },
                { "NoteToFrequency", { { val("MIDINote") }, { val("OutFrequency") } } },
                { "FrequencyToNote", { { val("Frequency") }, { val("OutMIDINote") } } },
            };
            return s_Pins;
        }
    } // namespace

    const NodePinSchema* GetNodePinSchema(const std::string& nodeType)
    {
        const auto& map = GetPinSchemaMap();
        auto it = map.find(nodeType);
        if (it == map.end())
            return nullptr;
        return &it->second;
    }

    f32 ParsePropertyFloat(const NodeParamSchema& schema, const std::string& valueStr)
    {
        if (valueStr.empty())
            return schema.DefaultFloat;
        try
        {
            return std::stof(valueStr);
        }
        catch (...)
        {
            return schema.DefaultFloat;
        }
    }

    i32 ParsePropertyInt(const NodeParamSchema& schema, const std::string& valueStr)
    {
        if (valueStr.empty())
            return schema.DefaultInt;
        try
        {
            return static_cast<i32>(std::stoi(valueStr));
        }
        catch (...)
        {
            return schema.DefaultInt;
        }
    }

    bool ParsePropertyBool(const NodeParamSchema& schema, const std::string& valueStr)
    {
        if (valueStr == "true" || valueStr == "1")
            return true;
        if (valueStr == "false" || valueStr == "0")
            return false;
        return schema.DefaultBool;
    }

    u64 ParsePropertyAssetHandle(const std::string& valueStr)
    {
        if (valueStr.empty())
            return 0;
        try
        {
            return std::stoull(valueStr);
        }
        catch (...)
        {
            return 0;
        }
    }

} // namespace OloEngine::Audio::SoundGraph
