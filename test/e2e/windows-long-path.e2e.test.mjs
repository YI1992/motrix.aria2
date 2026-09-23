// A download whose destination is longer than MAX_PATH (260) must open,
// write and complete (agalwood/Motrix#2183: a 262-character BT destination
// failed with "The system cannot find the path specified").
//
// aria2c passes such paths through the \\?\ namespace, so this must work
// without the LongPathsEnabled machine policy. Set LONG_PATHS_POLICY_OFF=1
// on Windows (CI does, after turning the policy off) to assert the policy is
// really off, proving the namespace, not the manifest, did the work. On
// other platforms the test is a plain deep-path regression.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { createHash, randomBytes } from 'node:crypto'
import { mkdtemp, rm, readFile, mkdir } from 'node:fs/promises'
import http from 'node:http'
import os from 'node:os'
import path from 'node:path'

import { Aria2Rpc } from './helpers/rpc-client.mjs'
import { startInstance, allocPorts, defaultAria2Bin } from './helpers/aria2-process.mjs'
import { waitFor } from './helpers/wait.mjs'

function longPathsPolicy() {
  const reg = path.win32.join(process.env.SystemRoot ?? 'C:\\Windows', 'System32', 'reg.exe')
  const out = execFileSync(
    reg,
    ['query', 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem', '/v', 'LongPathsEnabled'],
    { encoding: 'utf8' }
  )
  return /LongPathsEnabled\s+REG_DWORD\s+0x([0-9a-f]+)/i.exec(out)?.[1] ?? 'missing'
}

describe('download into a destination longer than MAX_PATH', () => {
  const payload = randomBytes(256 * 1024)
  const digest = createHash('sha256').update(payload).digest('hex')
  let workDir, server, handle, rpc, url

  before(async () => {
    if (process.platform === 'win32' && process.env.LONG_PATHS_POLICY_OFF === '1') {
      assert.equal(longPathsPolicy(), '0', 'LongPathsEnabled must be off for this proof')
    }
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-longpath-'))
    server = http.createServer((req, res) => {
      res.writeHead(200, { 'Content-Length': payload.length })
      res.end(req.method === 'HEAD' ? undefined : payload)
    })
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
    url = `http://127.0.0.1:${server.address().port}/track.flac`

    const ports = allocPorts()
    const sessionDir = path.join(workDir, 'session')
    await mkdir(sessionDir, { recursive: true })
    handle = await startInstance({
      ...ports,
      rpcSecret: 'long-path',
      dir: path.join(workDir, 'dl'),
      sessionPath: path.join(sessionDir, 'aria2.session'),
      sqliteDbPath: path.join(sessionDir, 'aria2.db'),
      logPath: path.join(sessionDir, 'aria2.log'),
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'long-path' })
  })

  after(async () => {
    await handle?.stop()
    server?.close()
    if (workDir && !process.env.ARIA2_E2E_KEEP_TMP) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('creates the nested directories and completes the file', async () => {
    // The shape of the #2183 report: a container, several nested folders and
    // a long track name, well past 260 characters in total.
    const dir = path.join(
      workDir,
      'dl',
      'Dead Cells (2018).motrix',
      'Dead Cells (2018)',
      'Bonuses',
      'Dead Cells - Demake Soundtrack',
      'FLAC Yoann Laulan - Dead Cells - Soundtrack Part 2 (Demake) FLAC'
    )
    const out =
      'Yoann Laulan - Dead Cells - Soundtrack Part 2 (Demake) - 21 The Time Keeper Formerly Known As Assassin.flac'
    const target = path.join(dir, out)
    assert.ok(target.length > 300, `destination is only ${target.length} characters`)

    const gid = await rpc.call('aria2.addUri', [[url], { dir, out }])
    const status = await waitFor(
      async () => {
        const s = await rpc.call('aria2.tellStatus', [gid, ['status', 'errorCode', 'errorMessage']])
        if (s.status === 'error') {
          throw new Error(`download failed: ${s.errorCode} ${s.errorMessage}`)
        }
        return s.status === 'complete' ? s : null
      },
      20_000,
      100,
      'long-path download to complete'
    )
    assert.equal(status.status, 'complete')
    // Node reaches long paths through its own \\?\ handling.
    const written = await readFile(target)
    assert.equal(createHash('sha256').update(written).digest('hex'), digest)
  })
})
