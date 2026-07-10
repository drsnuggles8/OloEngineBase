-- Example script-defined MCP tool (issue #357; docs/guides/mcp-diagnostics-server.md
-- § "Script-defined tools (Lua)", design in docs/adr/0005-mcp-script-tools-lua-sandbox.md).
--
-- Composes three read-only diagnostics into one call: is a scene loaded, are any
-- shaders broken, do any assets have problems. Handy first call when attaching an
-- agent to a session ("is anything obviously wrong?"). Also serves as the template
-- for writing your own: RegisterMcpTool{} + olo.call_tool() is the whole API.

RegisterMcpTool{
    name        = "script_health_digest",
    title       = "Project health digest",
    description = "One-call digest of session health: active scene summary, shader "
               .. "compile errors, and asset problems — composed from the read-only "
               .. "olo_* diagnostics tools.",
    schema      = { type = "object", properties = {}, additionalProperties = false },
    handler     = function(args)
        local digest = {}

        local scene, sceneErr = olo.call_tool("olo_scene_summary", {})
        digest.scene = scene or { error = sceneErr }

        local shaders, shaderErr = olo.call_tool("olo_shader_errors", {})
        if shaders ~= nil then
            digest.shaderErrors = shaders
        else
            digest.shaderErrors = { error = shaderErr }
        end

        local assets, assetErr = olo.call_tool("olo_assets_problems", {})
        if assets ~= nil then
            digest.assetProblems = assets
        else
            digest.assetProblems = { error = assetErr }
        end

        return digest
    end,
}
