import type { EngineInterface, Register } from 'claude-code'

import { gateFor, newTestFileNotes, normPath, readGateMessage, repoRoot } from './gates'

const FS_READ_LIMIT = 4 * 1024 * 1024

// Guides each loop (main, or a subagent by its id) has read this session. Module
// state: a reload of this mod forgets them, which costs one re-read.
const readByLoop = new Map<string, Set<string>>()

function loopOf(agentId: unknown): string {
  return typeof agentId === 'string' && agentId.length > 0 ? agentId : 'main'
}

function hasRead(loop: string, doc: string): boolean {
  const suffix = `/${doc}`
  return [...(readByLoop.get(loop) ?? [])].some(p => p.endsWith(suffix))
}

export const register: Register = on => {
  on('tool.call', { tool: 'Read' }, async ($, e, next) => {
    const ran = await next(e)
    if (ran.deny === undefined && ran.isError !== true && typeof e.file_path === 'string') {
      const loop = loopOf(e.agentId)
      const set = readByLoop.get(loop) ?? new Set<string>()
      set.add(normPath(e.file_path))
      readByLoop.set(loop, set)
    }
    return ran
  })

  on('tool.call', { tool: ['Edit', 'Write'] }, async ($, e, next) => {
    const path = typeof e.file_path === 'string' ? e.file_path : ''
    if (path.length === 0 || repoRoot(path) === undefined) {
      return next(e)
    }
    const exists = await $.fs.exists(path)
    const incoming = e.tool === 'Write' ? e.content : e.tool === 'Edit' ? e.new_string : undefined
    const needsText = /\.(?:h|hpp)$/i.test(path) && exists
    const text = needsText ? await readSmall($, path) : undefined
    const gate = gateFor(path, { isNew: !exists, text, incoming })

    if (gate.kind === 'deny') {
      return { deny: `olo-read-gates refused this edit: ${gate.reason}` }
    }
    if (gate.kind === 'requires-read' && !hasRead(loopOf(e.agentId), gate.doc)) {
      const root = repoRoot(path) ?? ''
      // A branch older than the guide has nothing to read: do not gate on a file that is not there.
      if (await $.fs.exists(`${root}/${gate.doc}`)) {
        return { deny: readGateMessage(gate, path) }
      }
    }

    const ran = await next(e)
    if (ran.deny !== undefined || ran.isError === true || exists || e.tool !== 'Write') {
      return ran
    }
    // A new test source was just written: is it built, and classified?
    const root = repoRoot(path) ?? ''
    if (!normPath(path).startsWith(`${normPath(root)}/oloengine/tests/`) || !path.toLowerCase().endsWith('.cpp')) {
      return ran
    }
    const notes = newTestFileNotes(
      path,
      e.tool === 'Write' ? e.content : '',
      await readSmall($, `${root}/OloEngine/tests/CMakeLists.txt`),
      await readSmall($, `${root}/OloEngine/tests/scripts/test_catalogue.json`),
    )
    return notes.length === 0 ? ran : { ...ran, context: [...(ran.context ?? []), ...notes] }
  })
    // A gate that breaks fails OPEN: these are reminders of the repo's rules, and a bug in
    // one must not block every edit in the session.
    .catch(($, e, next) => next(e))
}

async function readSmall($: EngineInterface, path: string): Promise<string | undefined> {
  try {
    const stat = await $.fs.stat(path)
    return stat.kind === 'file' && stat.size <= FS_READ_LIMIT ? await $.fs.read(path) : undefined
  } catch {
    return undefined
  }
}
