import { sleep } from './wait.mjs'

let nextId = 1

function toErrorMessage(value) {
  return value instanceof Error ? value.message : String(value)
}

export class WsRpcClient {
  constructor({ port, secret, rpcTimeoutMs = 5_000 }) {
    this.url = `ws://127.0.0.1:${port}/jsonrpc`
    this.secret = secret
    this.rpcTimeoutMs = rpcTimeoutMs
    this.socket = null
    this.pending = new Map()
    this.notifications = []
    this.notificationWaiters = new Set()
  }

  static async connect({
    port,
    secret,
    rpcTimeoutMs = 5_000,
    startupTimeoutMs = 10_000,
  }) {
    const deadline = Date.now() + startupTimeoutMs
    let lastError = new Error('WebSocket RPC did not become ready')

    while (Date.now() < deadline) {
      const client = new WsRpcClient({ port, secret, rpcTimeoutMs })
      try {
        await client.open(Math.min(1_000, deadline - Date.now()))
        await client.call('aria2.getVersion')
        return client
      } catch (error) {
        lastError = error
        await client.close()
        await sleep(100)
      }
    }

    throw new Error(
      `WebSocket RPC ${port} did not become ready after ${startupTimeoutMs}ms: ${toErrorMessage(lastError)}`
    )
  }

  open(timeoutMs = 5_000) {
    if (typeof WebSocket !== 'function') {
      throw new Error('This test requires the Node.js global WebSocket API')
    }

    return new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url)
      this.socket = socket

      const timer = setTimeout(() => {
        socket.close()
        reject(new Error(`WebSocket open timed out after ${timeoutMs}ms`))
      }, Math.max(1, timeoutMs))

      socket.addEventListener(
        'open',
        () => {
          clearTimeout(timer)
          resolve()
        },
        { once: true }
      )
      socket.addEventListener(
        'error',
        () => {
          clearTimeout(timer)
          reject(new Error(`WebSocket failed to open: ${this.url}`))
        },
        { once: true }
      )
      socket.addEventListener('message', (event) => {
        this.handleMessage(event.data)
      })
      socket.addEventListener('close', () => {
        this.rejectPending(new Error('WebSocket closed'))
      })
    })
  }

  call(method, params = [], timeoutMs = this.rpcTimeoutMs) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('WebSocket is not open'))
    }

    const id = `ws-e2e-${nextId++}`
    const body = {
      jsonrpc: '2.0',
      id,
      method,
      params:
        this.secret === undefined || this.secret === null || this.secret === ''
          ? params
          : [`token:${this.secret}`, ...params],
    }

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(
          new Error(
            `WebSocket RPC ${method} (${id}) timed out after ${timeoutMs}ms`
          )
        )
      }, timeoutMs)

      this.pending.set(id, { method, resolve, reject, timer })
      try {
        this.socket.send(JSON.stringify(body))
      } catch (error) {
        clearTimeout(timer)
        this.pending.delete(id)
        reject(error)
      }
    })
  }

  waitForNotification(method, predicate = () => true, timeoutMs = 10_000) {
    const existing = this.notifications.find(
      (notification) =>
        notification.method === method && predicate(notification)
    )
    if (existing) return Promise.resolve(existing)

    return new Promise((resolve, reject) => {
      const waiter = { method, predicate, resolve, reject, timer: null }
      waiter.timer = setTimeout(() => {
        this.notificationWaiters.delete(waiter)
        reject(
          new Error(
            `WebSocket notification ${method} timed out after ${timeoutMs}ms`
          )
        )
      }, timeoutMs)
      this.notificationWaiters.add(waiter)
    })
  }

  handleMessage(data) {
    let message
    try {
      message = JSON.parse(typeof data === 'string' ? data : String(data))
    } catch (error) {
      this.rejectPending(
        new Error(`WebSocket returned invalid JSON: ${toErrorMessage(error)}`)
      )
      return
    }

    if (typeof message?.id === 'string') {
      const pending = this.pending.get(message.id)
      if (!pending) return

      clearTimeout(pending.timer)
      this.pending.delete(message.id)
      if (message.error) {
        pending.reject(
          new Error(
            `WebSocket RPC ${pending.method} failed: ${message.error.code} ${message.error.message}`
          )
        )
      } else {
        pending.resolve(message.result)
      }
      return
    }

    if (typeof message?.method !== 'string') return
    this.notifications.push(message)
    for (const waiter of this.notificationWaiters) {
      if (waiter.method !== message.method || !waiter.predicate(message)) {
        continue
      }
      clearTimeout(waiter.timer)
      this.notificationWaiters.delete(waiter)
      waiter.resolve(message)
    }
  }

  rejectPending(error) {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer)
      pending.reject(error)
    }
    this.pending.clear()
  }

  async close() {
    const socket = this.socket
    this.socket = null
    if (!socket || socket.readyState === WebSocket.CLOSED) return

    await new Promise((resolve) => {
      const timer = setTimeout(resolve, 1_000)
      socket.addEventListener(
        'close',
        () => {
          clearTimeout(timer)
          resolve()
        },
        { once: true }
      )
      socket.close()
    })
  }
}
