// All DESCRIBE_NODE specializations live in NodeDescriptions.h so they're visible
// at the point each node's reflection-driven registration is instantiated (most
// importantly in NodeTypes.cpp's INIT_ENDPOINTS_FUNCS expansions). This TU just
// pulls the header in so any TU-local references the linker expects also resolve
// here. See the header for the full explanation.
#include "OloEngine/Audio/SoundGraph/Nodes/NodeDescriptions.h"
