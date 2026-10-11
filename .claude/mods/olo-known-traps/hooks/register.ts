import type { EngineInterface, Register } from 'claude-code'

import { formatTraps, matchTraps, type Trap } from './traps'

const REPEAT_WINDOW_MS = 20 * 60 * 1000

// Each trap is pointed out once per window: once is the help, every time is noise.
const lastShown = new Map<string, number>()

export const register: Register = on => {
  on('tool.call', async ($, e, next) => {
    const ran = await next(e)
    if (ran.deny !== undefined || typeof ran.text !== 'string' || ran.text.length === 0) {
      return ran
    }
    const context = {
      tool: e.tool,
      command: e.tool === 'Bash' || e.tool === 'PowerShell' ? e.command : '',
      text: ran.text,
      isError: ran.isError === true,
    }
    const traps = await unseen($, matchTraps(context))
    return traps.length === 0 ? ran : { ...ran, context: [...(ran.context ?? []), formatTraps(context, traps)] }
  })
}

async function unseen($: EngineInterface, traps: Trap[]): Promise<Trap[]> {
  if (traps.length === 0) {
    return traps
  }
  const now = await $.clock.now()
  return traps.filter(t => {
    const last = lastShown.get(t.id)
    if (last !== undefined && now - last < REPEAT_WINDOW_MS) {
      return false
    }
    lastShown.set(t.id, now)
    return true
  })
}
