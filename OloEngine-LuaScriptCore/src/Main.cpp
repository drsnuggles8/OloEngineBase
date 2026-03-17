#pragma once

namespace OloEngine
{
    // AnimationGraphComponent Lua binding (requires Sol2 integration)
    // When Sol2 bindings are active, register:
    //   local anim = entity:GetComponent("AnimationGraphComponent")
    //   anim:SetFloat("Speed", value)
    //   anim:SetBool("IsGrounded", value)
    //   anim:SetInt("State", value)
    //   anim:SetTrigger("Jump")
    //   anim:GetFloat("Speed")
    //   anim:GetCurrentState(layerIndex)
}
