-- Example WRITE-TIER script-defined MCP tool (issue #607; design in
-- docs/adr/0005-mcp-script-tools-lua-sandbox.md § "Consent & writes", usage in
-- docs/guides/mcp-diagnostics-server.md § "Script-defined tools (Lua)").
--
-- `writes = true` is the whole difference from a read-only script tool. It makes
-- this tool ProjectWrite, so:
--   * it lists with readOnlyHint:false and takes the SAME per-action write-consent
--     gate as a native write tool — refused outright when the MCP panel's "Agent
--     writes" is Disabled (the default), a modal in Prompt, straight through in
--     Allow all;
--   * and only THEN may its handler reach ProjectWrite tools through
--     olo.call_tool. A read-only script tool that tried the same call is refused
--     at the bridge — including by calling THIS tool, which is itself ProjectWrite.
--
-- What it does: flip the renderer into a cheap "lite" configuration in one call
-- (the perf-bisect macro you'd otherwise hand-run as three olo_renderer_settings_set
-- calls), and report each setting's PREVIOUS value so the agent can put it back.

local kLiteSettings = {
    { setting = "upscale",      value = "performance" },
    { setting = "depthprepass", value = "on" },
    { setting = "softshadows",  value = "pcf" },
}

RegisterMcpTool{
    name        = "script_perf_lite_mode",
    title       = "Renderer: lite mode",
    description = "Write-tier macro: switch the renderer to a cheap configuration "
               .. "(FSR1 performance upscale, depth prepass on, PCF shadows) in one "
               .. "call, for perf bisecting. Reports each setting's previous value so "
               .. "you can restore it with olo_renderer_settings_set. Requires MCP "
               .. "write consent.",
    writes      = true,
    toolset     = "script",
    schema      = { type = "object", properties = {}, additionalProperties = false },
    -- Optional display icons (SEP-973). A self-contained data: URI keeps the icon
    -- from depending on any network fetch; an https:// URL is equally valid.
    icons       = {
        { src = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9"
             .. "IjAgMCAyNCAyNCI+PHBhdGggZD0iTTEzIDIgMyAxNGg3bC0xIDggMTAtMTJoLTd6IiBmaWxsPSIjZjVhNjIzIi8+PC9zdmc+",
          mimeType = "image/svg+xml",
          sizes = { "any" } },
    },
    handler     = function(args)
        local applied = {}
        local failures = {}

        for _, entry in ipairs(kLiteSettings) do
            local result, err = olo.call_tool("olo_renderer_settings_set",
                                              { setting = entry.setting, value = entry.value })
            if result ~= nil then
                applied[#applied + 1] = result
            else
                failures[#failures + 1] = { setting = entry.setting, error = err }
            end
        end

        return {
            applied  = applied,
            failures = failures,
            note     = "Restore with olo_renderer_settings_set using each entry's previousValue.",
        }
    end,
}
