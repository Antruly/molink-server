# Changelog

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
