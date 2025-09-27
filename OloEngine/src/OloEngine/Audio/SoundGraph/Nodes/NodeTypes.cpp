#include "OloEngine/Core/Base.h"
#include "NodeTypes.h"
#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"

namespace OloEngine::Audio::SoundGraph
{
	/// Moving all constructors and Init() function calls for non-template nodes here
	/// to avoid recursive includes nightmare caused by trying to inline as much as possible.

	/// This macro should be used to define constructor and Init() function for node processor types
	/// that don't need to do anything in their constructor and Init() function
#define INIT_ENDPOINTS(TNodeProcessor) TNodeProcessor::TNodeProcessor(const char* dbgName, UUID id) : NodeProcessor(dbgName, id) { EndpointUtilities::RegisterEndpoints(this); }\
							void TNodeProcessor::Init() { EndpointUtilities::InitializeInputs(this); }

	/// This macro can be used if a node processor type needs to have some custom stuff in constructor
	/// or in its Init() function. In that case it has to declare 'void RegisterEndpoints()' and 'void InitializeInputs()'
	/// functions to be defined by this macro
#define INIT_ENDPOINTS_FUNCS(TNodeProcessor) void TNodeProcessor::RegisterEndpoints() { EndpointUtilities::RegisterEndpoints(this); }\
							void TNodeProcessor::InitializeInputs() { EndpointUtilities::InitializeInputs(this); }

	// Non-template nodes that use standard constructor/Init pattern
	// These don't need custom behavior so we use INIT_ENDPOINTS

	// Math nodes that are non-template (following Hazel's pattern)
	// Note: Template math nodes are handled in NodeTypeImpls.h

	// Generator nodes that need custom behavior use INIT_ENDPOINTS_FUNCS
	INIT_ENDPOINTS_FUNCS(Noise);
	INIT_ENDPOINTS_FUNCS(SineOscillator);
	INIT_ENDPOINTS_FUNCS(SquareOscillator);
	INIT_ENDPOINTS_FUNCS(SawtoothOscillator);
	INIT_ENDPOINTS_FUNCS(TriangleOscillator);
	
	// WavePlayer needs custom behavior for asset loading
	INIT_ENDPOINTS_FUNCS(WavePlayer);
	
	// Envelope nodes need custom behavior for state machines
	INIT_ENDPOINTS_FUNCS(ADEnvelope);
	INIT_ENDPOINTS_FUNCS(ADSREnvelope);

	// Trigger nodes need custom behavior for timing control
	INIT_ENDPOINTS_FUNCS(RepeatTrigger);
	INIT_ENDPOINTS_FUNCS(TriggerCounter);
	INIT_ENDPOINTS_FUNCS(DelayedTrigger);

	// Music nodes with custom conversion logic
	INIT_ENDPOINTS_FUNCS(BPMToSeconds);
	INIT_ENDPOINTS_FUNCS(FrequencyToNote);

} // namespace OloEngine::Audio::SoundGraph