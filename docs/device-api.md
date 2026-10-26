# 设备目录 API（v1）

设备 API 供 moLink 客户端注册、上报心跳、查询地址和管理域组。默认监听
`0.0.0.0:28430`，与只绑定回环的管理面（`28431`）分离。部署、配置和安全策略见
[server.md](server.md)。

- 传输：HTTP/1.1；配置 `--tls-cert/--tls-key` 后可用 HTTPS（TLS 上只宣告 HTTP/1.1）。
- 编码：请求与响应均为 UTF-8 JSON；`POST` 建议显式带
  `Content-Type: application/json`。
- 兼容：0.2.1 起注册、心跳和域组接口接受 JSON 请求体，同时保留查询参数写法；
  客户端 0.8.x 与更早版本仍可用。
- CORS：设备 API 对所有来源返回 `Access-Control-Allow-Origin: *`，桌面 UI 可直接
  调用地址查询；`OPTIONS` 返回 204。
- 请求体上限：128 KiB（可容纳 32 KiB 头像的 JSON 包装）。
- 身份：注册返回的 `token` 是设备凭据，服务端只保存其 SHA-256；`heartbeat`、
  `domain`、`unregister` 都需要 `id + token`。查询接口不需要凭据。

## 标识格式

| 名称 | 格式 | 说明 |
|---|---|---|
| 设备 ID | 9 位 `A-Z0-9`，去除易混淆的 `I/1/O/0` | 唯一；展示为 `ABC-DEF-GHI` |
| 域组码 | 同上，9 位 | 只能由服务端生成，注册时分配 |
| 设备 token | 32 位十六进制（16 字节随机数） | 只在注册响应中出现一次 |

设备 ID 与域组码都忽略输入中的 `-`、`_`、空格和大小写。

## 错误格式

非 2xx 响应：

```json
{
  "type": "error",
  "code": "invalid_token",
  "message": "unknown device or token"
}
```

限速与封禁响应额外带 `Retry-After`（秒），限速 JSON 还包含 `retry_after`。

| HTTP | code | 含义 |
|---|---|---|
| 400 | `bad_request` | 缺少必填字段 |
| 400 | `bad_port` | 注册端口的取值不在 1..65535 |
| 400 | `bad_domain` | 加入域组时未提供合法的 9 位码 |
| 400 | `bad_action` | `action` 不是 `join` / `rotate` / `leave` |
| 400 | `bad_client_ip` | 无法解析来源地址 |
| 401 | `invalid_token` | 设备不存在或 token 不匹配 |
| 403 | `ip_denied` | 来源不在服务的 IP 策略允许范围内 |
| 403 | `ip_banned` | 来源处于临时封禁中 |
| 403 | `invalid_invite` | 服务端配置了注册邀请码但请求未提供/不匹配 |
| 404 | `not_found` | 查询的设备 ID 不存在 |
| 404 | `unknown_domain` | 域组码不存在或已被撤销 |
| 429 | `rate_limited` | 触发限速 |
| 429 | `ip_quota` | 同一 IP 注册的设备数达到上限（默认 50） |
| 503 | `database_unavailable` | 数据库不可用 |
| 503 | `shutting_down` | 服务正在停止 |

## 限速

所有 `/v1/*` 请求先经过「每 IP 600 次/分钟、突发 120」的总限速，然后按接口细分：

| 接口 | 限速 |
|---|---|
| 注册 | 10 次/小时/IP，突发 3 |
| 心跳 | 6 次/分钟/设备，突发 3；240 次/分钟/IP |
| 域组操作 | 10 次/分钟/设备，突发 5 |
| 地址查询 / 域组列表 | 60 次/分钟/IP，突发 30 |
| 注销 | 10 次/分钟/IP，突发 5 |

具体数值可在管理控制台的「安全策略」中调整。

---

## POST /v1/devices/register

注册一台设备，建立目录记录、设备凭据和初始域组码。

请求体（JSON；也接受同名查询参数）：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `name` | string | 否 | 显示名称；去控制字符，最长 96，空值记为 `device` |
| `port` | int | 是 | 客户端直连端口，1..65535 |
| `version` | string | 否 | 客户端版本，最长 32 |
| `platform` | string | 否 | `windows` / `macos` / `linux` / `android` / `ios` / `web` / `unknown`；其他值忽略 |
| `avatar` | string | 否 | `preset:<name>`（≤64）或 `data:image/...;base64,...`（≤32 KiB）；其他值忽略 |
| `invite` | string | 视配置 | 服务端设置 `registration_token` 时必填 |

```sh
curl -s -X POST https://directory.example.com/v1/devices/register \
  -H 'Content-Type: application/json' \
  -d '{"name":"Office PC","port":28418,"version":"0.8.1","platform":"windows"}'
```

```json
{
  "device_id": "NM8698LBD",
  "display_id": "NM8-698-LBD",
  "token": "bae025e2...d976ae5",
  "domain": "E8XRGU7GF",
  "domain_valid": true,
  "server_time": 1792869596040,
  "public_url": ""
}
```

同一 IP 注册过多会返回 429 `ip_quota`。重新注册会得到新的设备 ID 与新域组码；
旧记录需通过管理面吊销。

---

## POST /v1/devices/heartbeat

建议每 30 秒一次。成功后设备进入在线状态；服务端按 `offline_after_seconds`
（默认 90 秒）判定离线。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 是 | 设备 ID |
| `token` | string | 是 | 注册返回的设备 token |
| `port` | int | 否 | 直连端口；省略或 0 时保留上次的值 |
| `version` | string | 否 | 空值保留原值 |
| `name` | string | 否 | 空值保留原值 |
| `platform` | string | 否 | 空值保留原值 |
| `avatar` | string | 否 | 空值保留原值 |

```sh
curl -s -X POST http://127.0.0.1:28430/v1/devices/heartbeat \
  -H 'Content-Type: application/json' \
  -d '{"id":"NM8698LBD","token":"...","port":28418}'
```

```json
{
  "domain": "E8XRGU7GF",
  "domain_valid": true,
  "server_time": 1792869596191
}
```

`domain_valid` 为 `false` 表示域组码已被其所有者撤销；设备仍保持字母码，但
无法再列出同组设备，直到加入新码或生成自己的新码。旧服务端返回 204 且无响应体。

---

## POST /v1/devices/domain

管理当前设备的域组（频道）。域组只用于发现与筛选，不替代连接验证码。

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string | 是 | 设备 ID |
| `token` | string | 是 | 设备 token |
| `action` | string | 是 | `join` / `rotate` / `leave` |
| `code` | string | join 时必填 | 要加入的 9 位域组码 |

- `join`：加入一个仍然有效的码。码不存在或已撤销时返回 404 `unknown_domain`。
- `rotate`：由服务端为当前设备签发新码。
  - 如果当前码属于该设备，旧码立即撤销：所有加入过它的成员在下次心跳收到
    `domain_valid=false`，查询返回 `unknown_domain`。
  - 如果当前码是别人的（设备是加入者），旧组保持有效；本设备退出该组并得到
    自己的新码。
- `leave`：退出域组，`domain` 变为空字符串。

```sh
# 加入同事分享的频道
curl -s -X POST http://127.0.0.1:28430/v1/devices/domain \
  -H 'Content-Type: application/json' \
  -d '{"id":"NM8698LBD","token":"...","action":"join","code":"E8XRGU7GF"}'

# 生成自己的新码（若拥有旧码，旧码作废）
curl -s -X POST http://127.0.0.1:28430/v1/devices/domain \
  -H 'Content-Type: application/json' \
  -d '{"id":"NM8698LBD","token":"...","action":"rotate"}'
```

成功响应：

```json
{"domain":"P8ZM4CS2R","domain_valid":true,"server_time":1792869596209}
```

---

## GET /v1/devices

按 ID 查询设备的连接地址。公开接口，不需要 token。

```sh
curl -s 'http://127.0.0.1:28430/v1/devices?id=NM8-698-LBD'
```

```json
{
  "id": "NM8698LBD",
  "display_id": "NM8-698-LBD",
  "name": "Office PC",
  "address": "",
  "peer_ip": "203.0.113.8",
  "port": 28418,
  "version": "0.8.1",
  "platform": "windows",
  "avatar": "preset:teal",
  "online": true,
  "created_ms": 1792869596040,
  "last_seen_ms": 1792869596191,
  "public_url": ""
}
```

批量查询用 `ids`（逗号分隔，最多 20 个，自动去重）：

```sh
curl -s 'http://127.0.0.1:28430/v1/devices?ids=NM8-698-LBD,YBD-NJQ-RV8'
```

```json
{
  "devices": [
    {
      "id": "NM8698LBD",
      "display_id": "NM8-698-LBD",
      "name": "Office PC",
      "platform": "windows",
      "avatar": "preset:teal",
      "online": true,
      "last_seen_ms": 1792869596191
    }
  ]
}
```

批量查询只返回存在的设备，不返回 404。`address` 是客户端上报的可选地址，
为空时使用 `peer_ip`。目录只负责寻址，不转发媒体。

---

## GET /v1/devices/domain

列出与调用者同域组的**其他**设备（最多 100 台，按最近心跳排序）。只有当前码
有效且调用者是成员时才能读取。

| 参数 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 设备 ID |
| `token` | 是 | 设备 token |
| `code` | 否 | 可显式指定域组码；必须与设备当前码一致 |

```sh
curl -s 'http://127.0.0.1:28430/v1/devices/domain?id=NM8698LBD&token=...'
```

```json
{
  "domain": "E8XRGU7GF",
  "devices": [
    {
      "id": "YBDNJQRV8",
      "display_id": "YBD-NJQ-RV8",
      "name": "Field Laptop",
      "platform": "windows",
      "avatar": "",
      "address": "",
      "peer_ip": "203.0.113.9",
      "port": 28419,
      "online": true,
      "last_seen_ms": 1792869596238
    }
  ],
  "server_time": 1792869596203
}
```

码已撤销或设备已退出时返回 404 `unknown_domain`。列表不含调用者自己。

---

## POST /v1/devices/unregister

注销设备并删除目录记录。之后必须重新注册才能恢复。

```sh
curl -s -X POST 'http://127.0.0.1:28430/v1/devices/unregister' \
  -H 'Content-Type: application/json' \
  -d '{"id":"NM8698LBD","token":"..."}'
```

成功返回 `204 No Content`；设备或 token 不匹配返回 401。

---

## GET /healthz

数据库连通性检查，供负载均衡与监控使用。

```json
{"status":"ok","service":"moLink Server","version":"0.3.3","database":"ok","devices":0}
```

数据库不可用时返回 503，`status` 为 `degraded`。`devices` 是占位字段（目前固定
为 0），设备统计请以管理控制台为准。

## 典型流程

```text
register ──► (id, token, domain)
   │
   ├─ heartbeat 每 30s ──► domain_valid
   │                          │
   │                          └─ false → 旧码已撤销
   ├─ join <别人的码> ──► 进入同一频道
   ├─ rotate ──► 新码（拥有旧码时旧码作废）
   └─ unregister ──► 删除记录
```

## 隐私说明

- 地址查询按 ID 公开：知道 9 位设备 ID 的人可以读到公网/局域网地址与端口，
  因此设备 ID 应按凭据对待。
- 域组列表需要设备 token + 同码成员身份，不提供全局设备枚举。
- 设备 token 只在注册时返回一次；服务端仅保存哈希，管理面也不会显示明文。
