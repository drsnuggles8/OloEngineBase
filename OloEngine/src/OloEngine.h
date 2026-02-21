#pragma once

// Only for use by applications, never include this anywhere in the engine codebase

#include "OloEngine/Core/Base.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Layer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Assert.h"

#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/FastRandom.h"

#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"
#include "OloEngine/Renderer/Camera/OrthographicCameraController.h"

#include "OloEngine/ImGui/ImGuiLayer.h"

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include "OloEngine/Project/Project.h"

// --Renderer-------------------
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include "OloEngine/Renderer/Camera/OrthographicCamera.h"
// ------------------------------

// --Audio-----------------------
#include "OloEngine/Audio/AudioEngine.h"
#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/AudioListener.h"
#include "OloEngine/Audio/AudioLoader.h"

// ------------------------------
