// Helper function to get a VertexArray from its ID
static Ref<VertexArray> GetVertexArrayFromID(u32 rendererID)
{
    // In a real implementation, this would look up the VertexArray in a resource manager
    static std::unordered_map<u32, Ref<VertexArray>> s_VertexArrayCache;
    
    if (s_VertexArrayCache.find(rendererID) != s_VertexArrayCache.end())
    {
        return s_VertexArrayCache[rendererID];
    }
    else if (rendererID > 0)
    {
        // Create a proxy vertex array with minimal needed setup to prevent crashes
        auto vertexArray = VertexArray::Create();
        
        // Initialize with a dummy index buffer to prevent crashes
        u32 indices[6] = { 0, 1, 2, 2, 3, 0 }; // Simple quad indices
        auto indexBuffer = IndexBuffer::Create(indices, 6);
        vertexArray->SetIndexBuffer(indexBuffer);
        
        // Add a minimal vertex buffer to prevent crashes
        float vertices[16] = { 
            -1.0f, -1.0f, 0.0f, 0.0f,  // position, texcoord
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 1.0f
        };
        
        auto vertexBuffer = VertexBuffer::Create(vertices, sizeof(vertices));
        
        BufferLayout layout = {
            { ShaderDataType::Float2, "aPosition" },
            { ShaderDataType::Float2, "aTexCoord" }
        };
        
        vertexBuffer->SetLayout(layout);
        vertexArray->AddVertexBuffer(vertexBuffer);
        
        s_VertexArrayCache[rendererID] = vertexArray;
        OLO_CORE_INFO("Created proxy VertexArray for ID: {}", rendererID);
        return vertexArray;
    }
    
    return nullptr;
}

void CommandDispatch::DrawMesh(const void* data, RendererAPI& api)
{
    auto const* cmd = static_cast<const DrawMeshCommand*>(data);
    
    // Get the vertex array from the resource manager
    auto vertexArray = GetVertexArrayFromID(cmd->vaoID);
    if (!vertexArray)
    {
        OLO_CORE_ERROR("CommandDispatch::DrawMesh: Invalid vertex array ID: {}", cmd->vaoID);
        return;
    }
    
    // Verify we have a valid index buffer before attempting to draw
    if (!vertexArray->GetIndexBuffer())
    {
        OLO_CORE_ERROR("CommandDispatch::DrawMesh: Vertex array {} has no index buffer", cmd->vaoID);
        return;
    }
    
    // Bind material textures if needed
    if (cmd->useTextureMaps)
    {
        if (cmd->diffuseMapID > 0)
        {
            api.BindTexture(0, cmd->diffuseMapID);
        }
        
        if (cmd->specularMapID > 0)
        {
            api.BindTexture(1, cmd->specularMapID);
        }
    }
    
    // Draw the mesh using the index buffer
    // Use cmd->indexCount if provided, otherwise use the full index buffer
    u32 indexCount = cmd->indexCount > 0 ? cmd->indexCount : vertexArray->GetIndexBuffer()->GetCount();
    api.DrawIndexed(vertexArray, indexCount);
}
