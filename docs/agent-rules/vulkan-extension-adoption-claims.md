# Check an extension's claims against `validusage.json` and `vk.xml` before building on them

Before adopting a Vulkan extension because an issue says it removes a constraint, verify the
claim against the pinned SDK. Two files answer almost every such question in seconds, and both
are on disk at `$VULKAN_SDK/share/vulkan/registry/`:

- **`validusage.json`** — every VUID, by struct and command. What the extension still requires.
- **`vk.xml`** — each extension's `depends` string, enums and commands. What it relates to.

An issue's framing is a hypothesis. These two are the contract.

## The rules

**A "takes an address instead of a handle" extension does not remove create-time usage bits.**
Grep `validusage.json` for the new struct before assuming it does. The phrasing to look for is
*"the buffer from which \<range\> was queried"* — it means an address still traces back to a
created buffer, and that buffer's usage is still checked.

**Adopting an extension can add requirements.** Taking a device address at all needs
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` at create time. Buffers that only ever staged a
copy will not have it.

**Never claim "this does not narrow the hardware floor" from extension B being present wherever
extension A is.** Read both `depends` strings. Two extensions shipped in the same SDK for the
same design are routinely independent in `vk.xml`, and one device exposing both proves nothing
about the set.

**A per-command flag that mirrors a create-time bit is usually a two-directional obligation, not
a hint.** Read both the "was created with" and the "was created without" VUID. Passing 0 is
wrong in one direction and passing the flag is wrong in the other.

**A clean validation run is only evidence if the layer implements the new VUIDs.** A brand-new
extension may have none yet. Check before citing silence:

```bash
python -c "import re;d=open(r'$VULKAN_SDK/Bin/VkLayer_khronos_validation.dll','rb').read();\
print(len(re.findall(rb'VUID-VkYourNewStructKHR',d)))"
```

## What this cost, once (#1179, 2026-09-11)

The issue proposed `VK_KHR_device_address_commands` partly to drop the create-time usage bits
that only the handle-based commands needed. It cannot: `VUID-VkBindIndexBuffer3InfoKHR-addressRange-13051`
still demands `INDEX_BUFFER_BIT`, `-VkDrawIndirect2InfoKHR-addressRange-13107` `INDIRECT_BUFFER_BIT`,
and `-VkDeviceMemoryCopyKHR-srcRange-13017`/`-dstRange-13018` the transfer pair. The conversion
added `SHADER_DEVICE_ADDRESS_BIT` to every staging and readback buffer instead. The payoff the
issue described is real but belongs to the heap-suballocation work it unblocks (#1180), where one
buffer is created once with the union of every bit and the per-resource decision disappears.

The extension did move two usages to command time — `STORAGE_BUFFER_USAGE` and
`TRANSFORM_FEEDBACK_BUFFER_USAGE`, each with an `UNKNOWN_` variant. Those are the two-directional
kind: mandatory when the backing buffer has the bit (`VUID-*-13122`), forbidden when it does not
(`VUID-*-13123`). The engine's two index-buffer kinds differ on it, so it could not be hardcoded.

The floor claim was the more expensive mistake, because it reached an ADR before it was checked.
"Every device lacking `VK_EXT_descriptor_heap` is already refused, and that is a strictly narrower
set" was carried from the issue body into ADR 0010 as established. `vk.xml` says the two
extensions are independent — neither `depends` string names the other — so a device can expose the
heap and not the address commands, and the contract row is a real narrowing. Caught in review,
after it was published.

See [component-serializer-codegen.md](component-serializer-codegen.md) for the same shape of
failure in a different subsystem: a claim about what is generated, trusted instead of checked.
