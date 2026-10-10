import type { EngineInterface, Register } from 'claude-code'

import { ancestors, gateFor, guessRoot, newTestFileNotes, normPath, readGateMessage, relativeTo, resolvePath } from './gates'

const FS_READ_LIMIT = 4 * 1024 * 1024

// Files each loop (main, or a subagent by its id) has read this session, resolved and lower
// case. Module state: a reload of this mod forgets them, which costs one re-read.
const readByLoop = new Map<string, Set<string>>()
// Checkout roots already found, by directory.
const rootByDir = new Map<string, string | undefined>()

function loopOf(agentId: unknown): string {
  return typeof agentId === 'string' && agentId.length > 0 ? agentId : 'main'
}

/** Did this loop read THIS checkout's copy of the guide (not another worktree's)? */
function hasRead(loop: string, guide: string): boolean {
  return readByLoop.get(loop)?.has(normPath(guide)) ?? false
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
    const path = typeof e.file_path === 'string' ? resolvePath(e.file_path) : ''
    const root = path.length === 0 ? undefined : await checkoutRoot($, path)
    if (root === undefined) {
      return next(e)
    }
    const exists = await $.fs.exists(path)
    const incoming = e.tool === 'Write' ? e.content : e.tool === 'Edit' ? e.new_string : undefined
    const needsText = /\.(?:h|hpp)$/i.test(path) && exists
    const text = needsText ? await readSmall($, path) : undefined
    const gate = gateFor(path, root, { isNew: !exists, text, incoming })

    if (gate.kind === 'deny') {
      return { deny: `olo-read-gates refused this edit: ${gate.reason}` }
    }
    if (gate.kind === 'requires-read') {
      const guide = `${root}/${gate.doc}`
      // A branch older than the guide has nothing to read: do not gate on a file that is not there.
      if (!hasRead(loopOf(e.agentId), guide) && (await $.fs.exists(guide))) {
        return { deny: readGateMessage(gate, path, root) }
      }
    }

    const ran = await next(e)
    if (ran.deny !== undefined || ran.isError === true || exists || e.tool !== 'Write') {
      return ran
    }
    // A new test source was just written: is it built, and classified?
    const rel = relativeTo(path, root) ?? ''
    if (!rel.startsWith('oloengine/tests/') || !rel.endsWith('.cpp')) {
      return ran
    }
    const notes = newTestFileNotes(
      path,
      root,
      e.content,
      await readSmall($, `${root}/OloEngine/tests/CMakeLists.txt`),
      await readSmall($, `${root}/OloEngine/tests/scripts/test_catalogue.json`),
    )
    return notes.length === 0 ? ran : { ...ran, context: [...(ran.context ?? []), ...notes] }
  })
    // A gate that breaks fails OPEN: these are reminders of the repo's rules, and a bug in
    // one must not block every edit in the session.
    .catch(($, e, next) => next(e))
}

/**
 * The checkout a file belongs to: the nearest directory holding a `.git` (a directory in the
 * main checkout, a file in a worktree, either nested under `.claude/worktrees/`), falling back
 * to the first top-level directory name when the file system cannot be asked.
 */
async function checkoutRoot($: EngineInterface, path: string): Promise<string | undefined> {
  const dirs = ancestors(path)
  for (const dir of dirs) {
    const key = normPath(dir)
    if (rootByDir.has(key)) {
      return rootByDir.get(key)
    }
    try {
      if (await $.fs.exists(`${dir}/.git`)) {
        for (const below of dirs.slice(0, dirs.indexOf(dir) + 1)) {
          rootByDir.set(normPath(below), dir)
        }
        return dir
      }
    } catch {
      break
    }
  }
  return guessRoot(path)
}

async function readSmall($: EngineInterface, path: string): Promise<string | undefined> {
  try {
    const stat = await $.fs.stat(path)
    return stat.kind === 'file' && stat.size <= FS_READ_LIMIT ? await $.fs.read(path) : undefined
  } catch {
    return undefined
  }
}
