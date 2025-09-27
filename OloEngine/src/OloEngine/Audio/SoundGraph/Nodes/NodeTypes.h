#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "MathNodes.h"
#include "GeneratorNodes.h"
#include "EnvelopeNodes.h"
#include "WavePlayer.h"
#include "TriggerNodes.h"
#include "ArrayNodes.h"
#include "MusicNodes.h"

// This file includes all node types - following Hazel's NodeTypes.h pattern
// Template implementations are in NodeTypeImpls.h
// Non-template implementations are in NodeTypes.cpp

namespace OloEngine::Audio::SoundGraph
{
	// All node types are included from their respective category files
	// This provides a single include point for all nodes, just like Hazel
	
} // namespace OloEngine::Audio::SoundGraph