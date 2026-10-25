# P2P 打洞、信令与中继

## 1. 为什么按 ID 直连经常失败

设备目录目前只做地址解析：客户端拿到对方的 `address:port` 后直接发起 TCP/WS
连接。这个地址来自设备注册/心跳时上报的本机地址，只有在以下条件同时成立时
才可用：

- 对端在公网可达，或双方在同一局域网；
- 对端没有处于对称 NAT / CGNAT 后面；
- 防火墙/路由器没有拦截入站连接。

家庭宽带、4G/5G、公司网络通常至少有一条不满足，所以「能查到设备、一直连不上」
是预期行为，不是目录故障。要解决它，需要：

1. **信令**：双方通过设备目录交换 ICE 参数；
2. **打洞**：用 STUN 发现公网映射并尝试直连（libjuice）；
3. **中继**：打洞失败时经 TURN 转发，保证可用性；
4. **数据面**：打洞/中继成功后用 UDP 承载，后续升级到 QUIC / HTTP/3。

## 2. 总体架构

```text
        ┌─────────────────────────────┐
        │      moLink Server          │
        │  ┌───────────────────────┐  │
        │  │ 信令（已实现）         │  │  offer / answer / candidates
        │  │ /v1/connect*          │  │  ← 设备 A、设备 B 轮询
        │  └───────────────────────┘  │
        │  ┌───────────────────────┐  │
        │  │ TURN / 中继（规划）    │  │  ICE 失败时的兜底
        │  └───────────────────────┘  │
        └──────────┬──────────────────┘
                   │ 注册 / 心跳 / 信令
        ┌──────────┴──────────┐
        │                     │
   ┌────┴────┐           ┌────┴────┐
   │ 设备 A  │◀── ICE ──▶│ 设备 B  │
   │ libjuice│  打洞/中继  │ libjuice│
   └─────────┘           └─────────┘
        └──── UDP：QUIC / HTTP/3（后续）────┘
```

目录只负责「交换参数」和「兜底转发」，不参与正常会话的数据流。

## 3. 服务端信令（已实现）

所有接口使用设备注册时拿到的 `id` + `token` 鉴权，限速与心跳共享
`rate_limiter`；会话只保存在内存中，默认 90 秒无活动过期，单设备最多 8 个
进行中的呼叫。

### 3.1 发起呼叫

```http
POST /v1/connect
Content-Type: application/json

{
  "id": "AAAA11111",
  "token": "<device token>",
  "target": "BBBB22222",
  "offer": "<juice_get_local_description 的 SDP>",
  "candidates": ["candidate:1 1 udp ..."]
}
```

返回：

```json
{
  "connect_id": "3f2a…",
  "expires_ms": 1792900000000,
  "server_time": 1792899910000,
  "to": { "id": "BBBB22222", "display_id": "BBB-B22-222", "name": "Office PC" }
}
```

服务端会校验目标设备存在且在线（按 `offline_after_seconds` 判定）。

### 3.2 被叫方轮询

```http
GET /v1/connect/pending?id=BBBB22222&token=<token>
```

每 300–500 ms 轮询一次。返回 `sessions[]`，每项包含 `connect_id`、`from`
（设备名称/头像/平台）、`offer` 与 `from_candidates`。被叫方在本地弹出
「是否接受连接」，或按现有验证码流程确认。

### 3.3 接受呼叫

```http
POST /v1/connect/answer
{
  "id": "BBBB22222",
  "token": "<token>",
  "connect_id": "3f2a…",
  "answer": "<SDP>",
  "candidates": ["candidate:2 1 udp ..."]
}
```

只有被叫方能 answer；重复 answer 返回 `409 connect_conflict`。

### 3.4 交换候选地址

```http
POST /v1/connect/candidates
{ "id": "...", "token": "...", "connect_id": "...",
  "candidates": ["candidate:3 1 udp ..."] }
```

双方都可以追加。`GET /v1/connect/status?id=&token=&connect_id=` 返回完整会话：

```json
{
  "session": {
    "connect_id": "3f2a…",
    "from": {...}, "to": {...},
    "offer": "...", "answer": "...",
    "from_candidates": [...], "to_candidates": [...],
    "answered": true, "closed": false, "expired": false,
    "created_ms": ..., "expires_ms": ...
  }
}
```

### 3.5 关闭

```http
POST /v1/connect/close
{ "id": "...", "token": "...", "connect_id": "..." }
```

任一方都可关闭；关闭后对方仍可读取状态（看到 `closed: true`），30 秒后清理。

### 3.6 错误码

| HTTP | code | 含义 |
|---|---|---|
| 400 | `bad_request` | 字段缺失、offer/candidate 超限 |
| 401 | `invalid_token` | 设备 ID/token 不匹配 |
| 403 | `connect_forbidden` | 非会话参与方 |
| 404 | `connect_not_found` / `unknown_device` | 会话或目标不存在 |
| 409 | `connect_conflict` / `connect_closed` / `connect_expired` / `device_offline` | 状态冲突 |
| 429 | `connect_limit` | 单设备并发的呼叫过多 |
| 503 | `directory_unavailable` | 数据库不可用 |

上限：offer/answer 64 KiB，每侧候选 64 条、单条 1 KiB。

## 4. 客户端接入 libjuice

目标依赖：<https://github.com/paullouisageneau/libjuice>（C 库，MIT）。

### 4.1 先说结论：上游 libjuice 不能用我们的 socket

上游 libjuice（截至 master）的三种并发模式都会**自己创建 UDP socket**：

- `JUICE_CONCURRENCY_MODE_POLL`：`conn_poll_registry_init()` 里
  `thread_init()` 起了一个轮询线程，`conn_poll_init()` 里
  `udp_create_socket()` 建 socket。名字叫 POLL，但并不是“由调用方驱动”。
- `JUICE_CONCURRENCY_MODE_THREAD`：每个 agent 一个线程。
- `JUICE_CONCURRENCY_MODE_MUX`：多个 agent 共用一个 socket，但 socket 仍由
  libjuice 创建和拥有；`juice_mux_listen()` 不接收外部 fd。

公开头里也没有 `juice_set_socket()` / `juice_get_socket()`。上游 issue #214
（“Add single-threaded mode”）讨论的正是这个问题，目前仍未实现。

所以：**原样使用 libjuice，数据收发会走 libjuice 的 socket 和线程，而不是
libuvcpp 的 `uv_udp_t`。** 这与“网络层统一用 libuv”的目标冲突。

### 4.2 方案：给 libjuice 加 external-socket 模式

libjuice 内部已经把连接层抽象成 `conn_mode_entry_t`（`conn_poll` /
`conn_mux` / `conn_thread` 各实现一份），再加一个模式是可行的，而且不需要
改动 ICE/STUN 状态机：

```c
typedef int (*juice_sendto_cb)(void *user_ptr, const void *data, size_t size,
                               const struct sockaddr *dst, socklen_t dstlen);

typedef struct juice_external_io {
    juice_sendto_cb sendto;   // 由调用方实现，通常是 uv_udp_send
    void *user_ptr;
} juice_external_io_t;

// 新增模式：不建 socket、不建线程
config.concurrency_mode = JUICE_CONCURRENCY_MODE_EXTERNAL;
config.external = &io;

// 调用方把 uv_udp_recv 收到的数据报交给 ICE 状态机
int juice_agent_input(juice_agent_t *agent, const void *data, size_t size,
                      const struct sockaddr *src, socklen_t srclen,
                      uint64_t now_ms);

// 调用方用 uv_timer 驱动重传/保活
void juice_agent_tick(juice_agent_t *agent, uint64_t now_ms);
```

内部实现要点：

- 新增 `src/conn_external.c`：`conn_send()` 调 `io->sendto`；
  `conn_get_addrs()` 返回调用方提供的本机地址（或由调用方传入候选）；
  不创建 socket、不创建线程、没有 registry。
- 把 `conn_poll_process_udp()` 里的“收包 → `agent_process_stun/ice`”路径
  抽成 `juice_agent_input()`，由调用方直接调用。
- 重传/保活/consent 刷新由 `juice_agent_tick()` 驱动，时间由调用方传入。
- TURN 也走同一个 socket：relay 数据报同样从 `uv_udp_recv` 进来、从
  `io->sendto` 出去，libjuice 只负责 STUN/TURN 封装。

这样网络 I/O 100% 在 libuv 循环里，libjuice 只是 ICE 状态机。实现方式二选一：

1. **公开 fork**（推荐先做）：`Antruly/libjuice`，新增模式 + 上述三个 API；
   libjuice 是 MPL-2.0，公开 fork 即可满足修改发布要求。用
   FetchContent 固定到 fork 的 tag。
2. 向上游提 PR；`conn_mode_entry_t` 的结构适合加模式，但上游接受时间不可控。

> 如果暂时不 patch，只能用“libjuice 自己的 socket + 线程，收到数据后通过
> `uv_async` 投递到 libuv 循环”的过渡方案。它能跑通，但不符合“UDP 由
> libuvcpp 驱动”的要求，只建议用来验证信令和 ICE 流程。

### 4.3 Agent 配置（external 模式）

```c
juice_config_t config = {0};
config.concurrency_mode = JUICE_CONCURRENCY_MODE_EXTERNAL;
config.external = &io;                    // sendto -> uv_udp_send
config.stun_server_host = "stun.example.org";
config.stun_server_port = 3478;
config.cb_state_changed = on_state;
config.cb_candidate = on_candidate;       // trickle
config.cb_gathering_done = on_gathering_done;
juice_agent_t* agent = juice_create(&config);
```

数据面不再使用 `juice_send()` / `cb_recv`：ICE 完成后，应用直接在**同一个
`uv_udp_t`** 上收发自己的协议（当前可以是 MOLK over UDP，后续换成 QUIC）。
收到数据报时先交给 `juice_agent_input()`；它返回“已消费”（STUN/TURN 控制包）
或“非控制包”（交给会话层）。

### 4.4 呼叫方

1. `juice_create` → `juice_gather_candidates(agent)`；
2. `cb_candidate` 里把候选地址缓存，`cb_gathering_done` 后：
   - `juice_get_local_description()` 得到 SDP；
   - `POST /v1/connect`（offer + 已收集的 candidates）；
3. 每 300–500 ms `GET /v1/connect/status`：
   - 有 `answer` → `juice_set_remote_description()`；
   - 有新的 `to_candidates` → `juice_add_remote_candidate()`；
   - 对方 `gathering_done`（可由服务端状态里的标记或最后一条候选后延迟判定）→
     `juice_set_remote_gathering_done()`；
4. 状态到 `JUICE_STATE_COMPLETED`：停止信令轮询，开始发送数据；
   `JUICE_STATE_FAILED` 或超时（建议 10–15 s）：`POST /v1/connect/close`，
   转中继。

### 4.5 被叫方

1. 轮询 `GET /v1/connect/pending`；
2. 用户接受后 `juice_create` + `juice_gather_candidates`；
3. `juice_set_remote_description(offer)`，逐条 `juice_add_remote_candidate`；
4. `juice_get_local_description` + `POST /v1/connect/answer`；
5. 后续候选用 `POST /v1/connect/candidates` 追加；
6. ICE 完成后进入数据传输。

### 4.6 数据面

- external-socket 模式下，`uv_udp_t` 从头到尾都是我们的：
  - 收包：`uv_udp_recv_cb` → `juice_agent_input()`；ICE 控制包被消费，
    其余交给 `session_transport`；
  - 发包：`uv_udp_send()`；ICE 阶段由 `io->sendto` 发出，数据阶段由会话层
    直接发出；
  - 定时器：`uv_timer_t` 每 50–100 ms 调 `juice_agent_tick()`，驱动重传、
    保活与 consent 刷新。
- 先实现一个 UDP `session_transport`（与现有 WS transport 并列），承载当前
  MOLK 协议；之后把同一套 socket/收发接口换成 QUIC：
  - ngtcp2 允许从任意数据报入口喂包（`ngtcp2_conn_read_pkt`），发送端用
    `ngtcp2_conn_writev_datagram` 产出后走 `uv_udp_send`；
  - HTTP/3 作为 QUIC 上的应用层协议承载现有 API/会话；
  - 此时 libjuice 只保留 ICE/STUN 状态机，数据完全由我们的 UDP socket 承载。

## 5. 中继（NAT 严格时）

### 5.1 推荐：TURN，而不是自研转发

libjuice 自带一个 TURN 服务端实现（`juice_server_create`，见
`include/juice/juice.h`），可作为 `molink-server` 的一个可选模块启动：

```c
juice_server_config_t config = {0};
config.bind_address = "0.0.0.0";
config.external_address = "<公网地址>";
config.port = 3478;
config.relay_port_range_begin = 49152;
config.relay_port_range_end = 65535;
config.credentials = &cred;
config.credentials_count = 1;
juice_server_t* turn = juice_server_create(&config);
```

客户端在 `juice_config_t.turn_servers` 里填入目录下发的临时凭据；libjuice 会
自动生成 relay candidate，打洞失败时流量经 TURN 转发，上层完全无感。

生产环境也可直接部署 **coturn**，目录只负责下发凭据：

```http
POST /v1/relay/credentials   (规划)
{ "id": "...", "token": "...", "connect_id": "..." }
→ { "urls": ["turn:relay.example.com:3478?transport=udp"],
    "username": "<expire>:<device>", "credential": "<hmac>", "ttl": 600 }
```

凭据按会话签发、短期有效，避免 TURN 被当作免费代理。

### 5.2 当前 WS 传输的兜底

现有会话数据面是 WebSocket。在 QUIC/UDP 数据面落地前，如果需要「内网也能连」，
可以在目录上加一个 WS 中继：双方分别与 `/v1/relay/{connect_id}` 建立 WS，服务端
按 `connect_id` 配对并双向转发二进制帧。它只作为过渡方案，QUIC 落地后不再需要。

## 6. 安全

- 信令接口全部要求设备 `id` + `token`，与会话验证码相互独立；接收方仍需在
  本地确认（现有「免密请求 / 验证码」逻辑不变）。
- 呼叫会话默认 90 秒过期、单设备 8 个并发、接口限速；`connect_id` 为 128 位
  随机值，不可枚举。
- TURN 凭据短期有效、按会话签发；relay 端口范围与配额应由部署方限制。
- 中继只转发加密后的数据；面向公网的 QUIC 建议启用 TLS 1.3 + 证书校验。
- 服务端不解析 SDP，也不保存候选地址之外的任何连接内容。

## 7. 分阶段落地

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 目录信令 API（offer/answer/candidates/close）、限速、过期、单测 | ✅ 已完成 |
| 2 | libjuice external-socket 模式（fork/PR #367）：不建 socket/线程，`juice_agent_input` + `juice_agent_tick` | ✅ 已完成（PR #367） |
| 3 | 客户端接入：`/ws/connect` 代理、`uvcpp_udp` + `uvcpp_timer`、信令轮询与自动应答 | ✅ 已完成 |
| 4 | UDP `session_transport`：ICE 完成后收发数据，1200 字节分片/重组，验证码/鉴权复用 | ✅ 已完成 |
| 5 | TURN 兜底：目录内嵌 `juice_server` 或 coturn + 凭据接口 | 待做 |
| 6 | QUIC/HTTP3 数据面：ngtcp2 直接使用同一个 UDP socket，替换 WS 数据通道 | 待做 |
| 7 | 多路径/迁移、统计与带宽自适应 | 后续 |

阶段 1–4 已完成并在三台机器上验证：本机 Linux、192.168.2.210 Win10、192.168.2.212
Win7 x86 都通过公网目录 `/v1/connect*` 信令建立 ICE，会话的 hello、欢迎、光标、
剪贴板和 MOLK 帧走 ICE/UDP（大关键帧分片），本机核心的 `/ws/connect` 把浏览器
WebSocket 与 UDP 会话桥接；ICE 失败时前端回退直连 WebSocket。

阶段 5/6 仍是后续：TURN 可用 coturn 或 libjuice 的 `juice_server`；QUIC/HTTP3 直接
复用同一个 UDP socket（`juice_transport` 的发送接口可替换成 ngtcp2 的 datagram
写出）。STUN 目前通过 `MOLINK_STUN_HOST` / `MOLINK_STUN_PORT` 配置，未配置时仅主机
候选；同一 NAT 的场景（含本次三台测试机）不需要 STUN。
