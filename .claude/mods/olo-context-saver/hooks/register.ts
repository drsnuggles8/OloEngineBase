import type { Register } from 'claude-code'

import { compressLog } from './compress'

// Tool results that carry build and test logs.
const LOG_TOOLS = new Set(['Bash', 'PowerShell', 'Read'])

type TextBlock = { type: 'text'; text: string }
type ToolResultBlock = { type: 'tool_result'; tool_use_id?: string; content?: string | TextBlock[]; is_error?: boolean }

export const register: Register = on => {
  // The row a tool result is stored as: what the model reads from now on. Hooks on
  // tool.call (olo-output-truth among them) have already seen the full text by then.
  on('session.append', ($, e, next) => {
    const origin = e.origin as { kind?: string; tool?: string }
    if (e.door !== 'tool-result' || origin.kind !== 'tool' || !LOG_TOOLS.has(String(origin.tool))) {
      return next(e)
    }
    let changed = false
    const content = e.message.content.map(block => {
      if (block.type !== 'tool_result') {
        return block
      }
      const result = block as ToolResultBlock
      if (typeof result.content === 'string') {
        const squeezed = compressLog(result.content)
        if (squeezed === undefined) {
          return block
        }
        changed = true
        return { ...result, content: squeezed.text }
      }
      if (Array.isArray(result.content)) {
        const parts = result.content.map(part => {
          if (part.type !== 'text') {
            return part
          }
          const squeezed = compressLog(part.text)
          if (squeezed === undefined) {
            return part
          }
          changed = true
          return { ...part, text: squeezed.text }
        })
        return { ...result, content: parts }
      }
      return block
    })
    return changed ? next({ ...e, message: { ...e.message, content: content as typeof e.message.content } }) : next(e)
  })
}
