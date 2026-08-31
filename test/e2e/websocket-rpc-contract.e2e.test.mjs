import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import { access, mkdir, mkdtemp, rm } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'

import {
  allocPorts,
  buildArgs,
  defaultAria2Bin,
  spawnAria2,
  stopInstance,
} from './helpers/aria2-process.mjs'
import { WsRpcClient } from './helpers/ws-rpc-client.mjs'

const SECRET = 'ws_contract_secret'
const PROCESS_ROUNDS = 3
const SEQUENTIAL_CALLS = 50
const PARALLEL_CALLS = 16

function notificationGid(notification) {
  return notification.params?.[0]?.gid
}

async function startPayloadServer() {
  const body = Buffer.alloc(1024 * 1024, 0x61)
  const server = createServer((request, response) => {
    response.statusCode = 200
    response.setHeader('content-type', 'application/octet-stream')
    response.setHeader('content-length', String(body.length))
    response.end(body)
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  const address = server.address()
  assert.ok(address && typeof address !== 'string')
  return {
    url: `http://127.0.0.1:${address.port}/payload.bin`,
    close: () =>
      new Promise((resolve) => {
        server.closeAllConnections?.()
        server.close(() => resolve())
      }),
  }
}

describe('WebSocket RPC release contract', () => {
  let rootDir
  let payloadServer

  before(async () => {
    await access(defaultAria2Bin())
    rootDir = process.env.ARIA2_E2E_WORK_DIR
      ? path.resolve(process.env.ARIA2_E2E_WORK_DIR)
      : await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-ws-contract-'))
    await mkdir(rootDir, { recursive: true })
    payloadServer = await startPayloadServer()
  })

  after(async () => {
    try {
      await payloadServer?.close()
    } catch {}
    if (!process.env.ARIA2_E2E_KEEP_TMP && rootDir) {
      await rm(rootDir, { recursive: true, force: true })
    }
  })

  it(
    'returns responses and notifications across persistent connections',
    { timeout: 90_000 },
    async () => {
      for (let round = 1; round <= PROCESS_ROUNDS; round += 1) {
        // Exercise both Motrix's authenticated production shape and the
        // unauthenticated minimal reproducer reported in #2027.
        const rpcSecret = round % 2 === 0 ? undefined : SECRET
        const roundDir = path.join(rootDir, `round-${round}`)
        const downloadDir = path.join(roundDir, 'downloads')
        const ports = allocPorts()
        await mkdir(downloadDir, { recursive: true })

        const args = buildArgs({
          rpcPort: ports.rpcPort,
          rpcSecret,
          listenPort: ports.listenPort,
          dhtListenPort: ports.dhtListenPort,
          dir: downloadDir,
          sessionPath: path.join(roundDir, 'aria2.session'),
          sqliteDbPath: path.join(roundDir, 'aria2.db'),
          logPath: path.join(roundDir, 'aria2.log'),
          logLevel: 'debug',
          extra: [
            '--no-conf=true',
            '--enable-dht=false',
            '--enable-peer-exchange=false',
          ],
        })

        const proc = await spawnAria2({
          bin: defaultAria2Bin(),
          args,
          silent: true,
        })
        let client
        try {
          client = await WsRpcClient.connect({
            port: ports.rpcPort,
            secret: rpcSecret,
          })

          const version = await client.call('aria2.getVersion')
          assert.equal(typeof version.version, 'string')
          assert.equal(
            await client.call('aria2.changeGlobalOption', [
              { continue: 'true' },
            ]),
            'OK'
          )

          for (let index = 0; index < SEQUENTIAL_CALLS; index += 1) {
            const stat = await client.call('aria2.getGlobalStat')
            assert.equal(typeof stat.numActive, 'string')
          }

          const parallel = await Promise.all(
            Array.from({ length: PARALLEL_CALLS }, () =>
              client.call('aria2.getVersion')
            )
          )
          assert.equal(parallel.length, PARALLEL_CALLS)
          assert.ok(parallel.every((result) => result.version === version.version))

          const gid = await client.call('aria2.addUri', [
            [payloadServer.url],
            { dir: downloadDir, out: `round-${round}.bin` },
          ])
          assert.equal(typeof gid, 'string')

          await client.waitForNotification(
            'aria2.onDownloadStart',
            (notification) => notificationGid(notification) === gid
          )
          await client.waitForNotification(
            'aria2.onDownloadComplete',
            (notification) => notificationGid(notification) === gid
          )

          await client.close()
          client = await WsRpcClient.connect({
            port: ports.rpcPort,
            secret: rpcSecret,
          })
          const reconnectedVersion = await client.call('aria2.getVersion')
          assert.equal(reconnectedVersion.version, version.version)
        } finally {
          await client?.close()
          await stopInstance(proc)
        }
      }
    }
  )
})
