# Changelog

## [0.3.5] — 2026-10-27

### Fixed

- TCP 中继在写入途中关闭会话时，在途消息的完成回调没有被结算，全局
  `pending_bytes` 每关一次会话永久多出约 64 KiB；约 2048 次后触发 128 MiB
  上限，所有中继开始丢包（日志 `relay backlog full`）而实际队列为空。现在
  `tcp_relay_link` 保存 `in_flight_done_` 并在 `fail_pending()` 中结算。
- 重新启用 HTTP/3 中继端点（默认 `28444`）：libuvcpp 1.8.8 为
  extended-CONNECT 流的发送队列加了 4 MiB 上限（超出返回 `UV_ENOBUFS`），
  接收端停止读取时不会再无限增长到 OOM。`/v1/connect/relay` 重新宣告
  `http3`，`preferred` 顺序为 QUIC → HTTP3 → TCP → HTTP2 → UDP → WebSocket。
- 构建改用 libuvcpp 1.8.8，包含 TLS 客户端析构时 `uvcpp_ssl` 的每连接泄漏修复
  （旧版每个 TLS 连接泄漏约 50–58 KiB，空闲服务每分钟涨 12–14 MB）。

## [0.3.4] — 2026-10-27

### Fixed

- 修复中继内存无限增长：libuvcpp 的 HTTP/3 流发送队列（`out_pending`）没有
  上限，对端停止读取时服务端会把数据一直攒到进程被 OOM kill；HTTP/3 中继
  端点的 done 回调又是立即返回，绕过了 `relay_hub` 的 4 MiB/128 MiB 背压
  上限。现在**关闭 HTTP/3 中继端点**并从 `/v1/connect/relay` 的 endpoints /
  preferred 中移除（启动日志会说明原因）；QUIC、TCP、HTTP/2、WebSocket
  仍有各自的队列上限，客户端会自动改用它们。等 libuvcpp 给 h3 流加上发送
  队列上限后再恢复。
- `/v1/connect/relay` 的 `preferred` 现在只列出本实例实际提供的协议，不再
  指向未启用的端点。
- 中继配对会话增加 8192 个的硬上限，避免大量未配对的 `POST /v1/connect/relay`
  在 10 分钟清理窗口内把 `sessions_` 撑大；`/healthz` 与 `/api/health` 新增
  `relay` 统计（sessions / bound / pending_bytes / dropped），便于确认中继
  积压。

## [0.3.3] — 2026-10-27

### Fixed

- macOS 构建失败：`relay_quic.cpp` 无条件包含 libuvcpp 的 QUIC 头，而 macOS
  CI 从源码构建的 libuvcpp 没有 ngtcp2/nghttp3。现在与 HTTP/3 一样按头文件
  是否存在决定：缺少 QUIC 时使用无实现的端点，`relay_quic_listening()` 为假，
  `/v1/connect/relay` 不宣告 quic，其余中继照常可用。
- 本地验证 QUIC stub 单独编译通过，真实 QUIC 构建与 5/5 测试通过。

0.3.2 的 Linux/Windows 链接修复见下一节。

## [0.3.2] — 2026-10-27

### Fixed

- Linux 构建链接失败：预编译 libuvcpp 的 `uvcpp.pc` 未在 `Requires` 中声明
  libuv，而 UDP 中继端点直接调用 `uv_*`，导致 `undefined reference to
  uv_udp_recv_stop / uv_ip4_addr / ...`。现在 pkg-config 路径会显式查找并链接
  系统 libuv；预编译归档路径也会链接系统 libuv。
- Windows 构建：1.8.6 的 MSVC 归档把 libuv 静态链进 `uvcpp.dll`，没有
  `uv.lib`；直接再链一个 libuv 会变成两份运行时。现在检测不到 `uv.lib` 时
  用无实现的 UDP 端点替代，其余中继（WS / TCP / QUIC / HTTP3 / HTTP2）照常
  可用；`relay_udp_listening()` 为假，`/v1/connect/relay` 不会宣告 udp。
- 本地用“去掉 `Requires: libuv` 的 uvcpp.pc”强制走 pkg-config 路径复现并验证
  了完整构建。

0.3.1 的中继数据面功能见下一节。

## [0.3.1] — 2026-10-27

### Added

- 中继数据面支持 TCP：`--relay-tcp-port`（默认 `28441`，0 关闭）在设备证书下
  接受 TLS 连接，握手是一行 `{"connect_id":"...","token":"..."}`，之后与 WS
  中继完全相同的配对、`relay-ready` 与字节转发。`POST /v1/connect/relay`
  现在返回 `relay.endpoints`（协议/端口/是否 TLS），按
  `quic → tcp → websocket` 排序；同一 `relay_token` 可打开任一端点。
- 中继数据面支持 QUIC：`--relay-quic-port`（默认 `28442`）在设备证书下监听，
  ALPN 为 `molink-relay`（与桌面直连的 `molink` 区分），一条双向流跑同一行握手
  与 MOLK 字节流；ready 行与 `/api` 状态都会报告它。
- 中继数据面支持 UDP：`--relay-udp-port`（默认 `28443`）按源地址配对两个设备，
  数据报原样转发；首包是同一行 JSON 握手，配对后提示也是 JSON 数据报（以 `{`
  开头，与 0x00/0x01/0x02 数据标签不冲突）。空闲映射 5 分钟后清理。数据报不
  可靠，丢包由 MOLK 的 tile-clock 修复兜底。
- 中继内部抽成与协议无关的 `relay_link`，WS 与流式端点共用配对、积压上限和
  关闭通知。
- 设备注册/心跳接受 `quic_port` 与 `tcp_port`（schema v6），
  `GET /v1/devices?id=` 在请求方与目标同公网 IP（或回环）时一并返回
  `address`/`port`/`quic_port`/`tcp_port`，供内网设备直接协商数据通道；
  外网查询仍然不泄露这些字段。

- 中继数据面支持 HTTP/3：`--relay-http3-port`（默认 `28444`）在设备证书下以
  ALPN `h3` 监听，接受 extended CONNECT（`:protocol=websocket`）；首条文本消息
  是同一行 JSON 握手，之后按 WS 消息原样转发（可靠流，无需分片）。要求
  libuvcpp 带 HTTP/3 WebSocket API；缺失时该端点自动报不可用。
- 中继数据面支持 HTTP/2 WebSocket（RFC 8441）：设备 web 端口开启 h2（TLS 时
  ALPN `h2`，无证书时 h2c 先验知识），同一 `/v1/relay` 路径注册 extended
  CONNECT 路由；首条文本消息是同一行 JSON 握手，之后按 WS 消息原样转发。
  `POST /v1/connect/relay` 宣告 `protocol:"http2"` 端点，复用设备端口与
  `secure` 标志。本机 h2c 与 TLS/h2 双端回环均验证 relay-ready、文本和二进制
  双向转发。
- `relay.endpoints` 顺序更新为
  `quic → http3 → tcp → http2 → udp → websocket`。

- 设备证书下发：schema v7 给 `devices` 增加 `tls_fingerprint`/`tls_cert`，
  注册/心跳接受这两个字段；`GET /v1/devices?id=` 只在同公网 IP（或回环）时
  返回它们，供客户端做 LAN 直连固定；`POST /v1/connect/relay` 也带
  `tls_cert`，便于未配置 CA 的客户端固定中继 TLS。

### Fixed

- 中继链路不再无限增长内存（0.3.0 线上 OOM 的根因）：libuvcpp 的 WebSocket
  发送队列没有上限，一侧设备停止读取（弱网、挂起的标签页、休眠的笔记本）时，
  服务端会一直把对端发来的帧排进该连接的队列，直到进程被 OOM kill。现在每个
  中继方向有 4 MiB 积压上限、全进程中继积压 128 MiB，超限的消息直接丢弃并限频
  记日志；接收端恢复后靠 MOLK 的 tile-clock 补帧。实测对一个完全不读的接收端
  灌入 3.2 GB，服务端 RSS 稳定在 24 MiB（结束后 16.8 MiB），日志按 5 秒限频
  报告丢弃数量。

## [0.3.0] — 2026-10-25

### Added

- P2P 中继数据面：`POST /v1/connect/relay` 用设备令牌换取每会话中继令牌，
  `WS /v1/relay?connect_id=&token=` 把同一信令会话的两个设备配对并转发文本/二进制消息；
  两端都连上时下发 `relay-ready`，一侧断开时通知另一侧，`/v1/connect/close` 会同时关闭中继。
  中继跑在现有的 TLS 端口上，不需要新的防火墙端口。

moLink Server 使用本仓库的 `v*` 标签发布，与桌面客户端
（[Antruly/molink](https://github.com/Antruly/molink)）的版本线相互独立。

## [0.2.2] — 2026-10-25

### Added

- 默认启用 HTTPS：首次启动在 `cer/` 下自动生成自签名证书（覆盖主机名、
  `localhost` 与本机地址），设备面与管理面共用一套证书；`--no-tls` 可关闭
- 管理页「设置 → HTTPS 证书」：查看主题/有效期/SAN/指纹，关闭证书、上传自己的
  PEM 证书与私钥、重新生成自签名证书；修改写回配置文件，重启服务后生效
- 安装目录附带 `config.example.json`，默认把证书与私钥指向同目录的 `cer/`
- 配置新增 `tls` 段（`enabled` / `cert` / `key` / `auto_self_signed`）；
  `MOLINK_SERVER_TLS` 环境变量
- 注册与心跳接受客户端上报的局域网地址；`GET /v1/devices` 仅在请求方与目标共享
  同一公网 IP（或请求来自回环）时返回该地址，外网查询不会泄露内网地址
- P2P 呼叫信令：`/v1/connect`、`/v1/connect/pending`、`/v1/connect/answer`、
  `/v1/connect/status`、`/v1/connect/candidates`、`/v1/connect/close`；
  内存会话、90 秒过期、单设备 8 个并发、设备凭据鉴权与限速。设计见
  `docs/p2p-relay.md`

### Changed

- 相对证书路径改为以 `config.json` 所在目录为基准
- 旧的 `tls/fullchain.pem` + `privkey.pem` 目录约定保留，仅在 `cer/` 不存在时使用

## [0.2.1] — 2026-10-25

### Added

- 设备记录新增平台、头像与域组码（迁移 5）；注册与心跳接口接受 JSON 请求体
- 域组码：注册时为每台设备生成唯一 9 位码；`POST /v1/devices/domain` 支持
  join / rotate / leave，`GET /v1/devices/domain` 列出同域组设备；rotate 由码的
  所有者撤销旧码，旧成员在下次心跳收到 `domain_valid=false`，查询返回
  `unknown_domain`
- `GET /v1/devices?ids=` 批量查询（最多 20 个 ID）
- 管理控制台显示设备平台与所属域组码
- 未指定 `--config` / `MOLINK_SERVER_CONFIG` 时自动读取当前目录的 `config.json`
- 新增 `MOLINK_SERVER_TLS_CERT` / `MOLINK_SERVER_TLS_KEY`（及管理面对应变量）
  环境变量
- 目录约定：把 `fullchain.pem`/`privkey.pem`（或 `cert.pem`/`key.pem`）放进
  `<配置目录>/tls/` 或 `<data_dir>/tls/`，启动时自动启用 TLS

### Changed

- 心跳返回 200 + `{domain, domain_valid}`（原先为 204），客户端可即时发现域组码失效
- 设备 API 请求体上限从 64 KiB 提升到 128 KiB（头像）
- 心跳省略名称时不再把已存名称覆盖为 `device`

## [0.2.0] — 2026-10-25

### Added

- 设备目录迁入数据库：默认 SQLite（WAL、连接池、事务化迁移），可选 PostgreSQL
  （运行时加载 libpq）；旧 `devices.json` 首次启动自动导入并保留原文件
- Web 管理控制台（独立 SPA）：首次初始化、登录、仪表盘、设备、管理员、安全策略、
  封禁、审计与设置；亮/暗主题，随服务端 `web-admin` 目录发布
- 多账户与角色（admin / operator / viewer）；HttpOnly 会话 Cookie + CSRF 双提交、
  Origin 校验、PBKDF2-HMAC-SHA256（600000 轮）、最后一个管理员保护
- IP 来源策略：私有 / 中国大陆 / 海外 + 自定义允许/拒绝 CIDR；内置 APNIC 中国
  IPv4 表；默认仅私有 + 中国大陆
- 请求限速（设备 API、注册、心跳、查询、管理 API）、登录失败阶梯与临时封禁；
  受信代理 `X-Forwarded-For`；审计事件
- 设备 API 与管理面分端口（28430 / 127.0.0.1:28431），管理面可选 TLS 或反向代理
- 三平台打包：Linux deb/rpm/tar.gz + systemd（安装即启用）、Windows 安装器/zip +
  可选 Windows 服务、macOS 未签名 pkg/tar.gz + LaunchDaemon
- libuvcpp 以 DB + OpenSSL + PostgreSQL 运行时加载构建，并在 CI 中断言这些开关
  确实生效

### Known limitations

- macOS pkg 未签名/未公证；Windows 包在干净系统上可能需要 VC++ Redistributable
- 中国 IPv6 段未内置；公网 IPv6 按海外处理
- 分布式/多节点管理与中继属于第三阶段（Pro），见 `docs/server-distributed.md`
