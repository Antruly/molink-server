# moLink Server 部署与运维

`molink-server` 是 moLink 二阶段的设备目录服务：桌面客户端注册设备、上报心跳，
客户端通过设备 ID 查询地址后直连；同时提供独立的 Web 管理控制台、账号体系、
IP 来源策略、请求限速、临时封禁和审计日志。

本仓库独立于桌面端发布，使用 `v*` 标签；当前版本 `0.3.2`。设备 API 的字段、
错误码、限速与请求示例见 [device-api.md](device-api.md)。

## 1. 端口与组成

| 监听 | 默认地址 | 用途 |
|---|---|---|
| 设备 API | `0.0.0.0:28430` | `/v1/devices/register|heartbeat|domain|unregister`、`/v1/devices`、`/v1/connect*`（P2P 信令）、`/healthz` |
| 管理 API/网页 | `127.0.0.1:28431` | `/api/admin/*` 与内置管理控制台 |

管理面默认只绑定回环地址，安装时可选择绑定 `0.0.0.0`。公网部署时建议：

- 管理面保持回环，通过 SSH 隧道或本机浏览器访问；或
- 绑定内网/公网时配置 TLS（`admin.tls_cert/key`）或放在 HTTPS 反向代理后。

设备面与桌面客户端保持兼容：接口路径、参数与返回字段不变，新增的拒绝码包括
`ip_denied`、`ip_banned`、`rate_limited`、`unknown_domain`。

### 域组（频道）码

注册成功时目录会为设备生成一个唯一的 9 位域组码（字符集去掉了易混淆的
`I/1/O/0`），并在注册响应与每次心跳中返回 `domain` / `domain_valid`。域组码只能
由目录生成，客户端不能自定义。

- `POST /v1/devices/domain`，JSON 体 `{id, token, action, code?}`：
  - `join`：加入 `code` 指定的域组；码不存在或已撤销时返回 404 `unknown_domain`
  - `rotate`：为当前设备签发新码。若当前码属于该设备，旧码立即撤销；旧成员下次
    心跳收到 `domain_valid=false`，查询返回 `unknown_domain`。若设备只是加入者，
    旧组保持不变，本设备退出并获得自己的新码
  - `leave`：退出域组，不再出现在同组列表中
- `GET /v1/devices/domain?id=<ID>&token=<TOKEN>`：返回与调用者同域组的其他设备
  （最多 100 台，含在线状态与连接地址）。非成员或已撤销的码返回 404。
- 域组码用于筛选与发现，不替代验证码：实际连接仍需要对方验证码或访问确认。

### 1.1 局域网地址

注册与心跳可以携带 `address`（客户端访问目录所用的本机 IPv4）。目录保存它，并在
`GET /v1/devices` 中返回：只有请求方与目标**共享同一个公网 IP**（同一 NAT 出口，
或请求来自回环）时，响应里的 `address` 才非空；外网查询只返回 `peer_ip`。客户端
优先使用 `address`，这样同一内网里的设备通过公网目录也能直接互联，同时不会把
内网地址暴露给外网。

## 2. 安装

### Linux

提供 `.deb`、`.rpm` 与 `.tar.gz`。deb/rpm 安装后会自动创建 `molink-server`
系统用户、数据目录并 `systemctl enable --now`：

```sh
sudo dpkg -i molink-server_0.3.2_amd64.deb
# 或
sudo rpm -i molink-server-0.3.2-1.x86_64.rpm

systemctl status molink-server
journalctl -u molink-server -f
```

tar.gz 解压后以 root 运行：

```sh
./install.sh                 # 安装文件、创建用户、注册并启动 systemd
./install.sh --no-service    # 只安装文件，不注册服务
./install.sh --no-start      # 注册服务但不立即启动
./uninstall.sh               # 保留配置和数据
./uninstall.sh --purge       # 连同配置、数据和服务账户一起删除
```

文件布局：

| 路径 | 内容 |
|---|---|
| `/opt/molink-server/bin/molink-server` | 可执行文件 |
| `/opt/molink-server/lib` | 随包的 libuvcpp/libllhttp |
| `/opt/molink-server/share/web-admin` | 管理控制台静态文件 |
| `/etc/molink-server/config.json` | 配置文件（deb conffile / rpm noreplace） |
| `/var/lib/molink-server` | SQLite 数据库等数据（0700，`molink-server` 用户） |
| `/var/log/molink-server/server.log` | 运行日志（20 MiB × 5 轮转） |
| `/usr/share/icons/hicolor/{scalable,128x128,256x256}/apps/molink-server.*` | 服务端图标 |
| `/usr/share/applications/molink-server.desktop` | 「moLink Server 管理控制台」快捷方式 |

Linux 包依赖系统 `libssl3`、`libuv1`、`zlib1g`；PostgreSQL 客户端库按需安装
（`libpq5`），仅当选择 PostgreSQL 驱动时用到。

### Windows

- 安装器 `moLink-Server-0.3.2-setup-x64.exe`：可选“安装为 Windows 服务”（默认勾选）、
  可选防火墙放行 28430、可选管理面绑定 `0.0.0.0`。配置写入
  `C:\ProgramData\moLink\Server\config.json`，卸载时保留数据。
- 便携 zip：解压到固定目录后运行 `molink-server.exe --config config.json`；
  以管理员运行 `install-service.cmd` 注册服务。

服务名 `moLinkServer`，自动（延迟）启动；日志与数据库默认在
`C:\ProgramData\moLink\Server`。安装包与开始菜单快捷方式使用 `molink.ico`
（服务端图标）。包内尽可能附带 OpenSSL 与 VC++ 运行库；
若目标机器提示缺少 `VCRUNTIME140`/`MSVCP140`，安装 Microsoft Visual C++
Redistributable 即可。

### macOS

- `moLink-Server-0.3.2-arm64.pkg` / `x64.pkg`（未签名）与对应的
  `macos-<arch>.tar.gz`。
- pkg 安装到 `/usr/local/molink-server`，创建 `_molink` 用户，注册
  `/Library/LaunchDaemons/com.molink.server.plist` 并启动。
- 配置 `/usr/local/etc/molink-server/config.json`，数据
  `/usr/local/var/molink-server`，日志 `/usr/local/var/log/molink-server`。
- 图标随包安装到 `/usr/local/molink-server/share/icons/`。

未签名 pkg 受 Gatekeeper 限制：可右键“打开”，或在终端执行
`xattr -d com.apple.quarantine <pkg>`。正式分发需要 Developer ID 与公证，后续补。

服务管理：

```sh
sudo launchctl kickstart -k system/com.molink.server
sudo launchctl bootout system/com.molink.server
```

## 3. 首次使用

浏览器打开 `https://127.0.0.1:28431`（默认自签名证书）：

1. 设置管理员用户名、显示名称和密码（至少 10 个字符）。
2. 创建后进入仪表盘，显示在线/注册设备、账户、封禁、运行时间与监听信息。
3. 左侧菜单：仪表盘、设备、管理员、安全策略、封禁、审计、设置。

如果管理面绑定在非回环地址且还没有管理员，服务端会在启动日志中打印一次性
初始化令牌；在初始化表单中填入即可。也可以通过 `--setup-token` 固定该令牌。

账户角色：

| 角色 | 权限 |
|---|---|
| admin | 全部，包括账户、安全策略和服务设置 |
| operator | 查看、吊销设备、管理封禁、查看审计 |
| viewer | 只读 |

## 4. 配置

优先级：**命令行 > 环境变量 > 配置文件 > 内置默认值**。配置文件为 JSON，
通过 `--config` 指定；环境变量 `MOLINK_SERVER_CONFIG` 也可给出路径。两者都
没有给出时，服务会读取**当前工作目录下的 `config.json`**（便携包/双击启动的
场景）；仍找不到就只使用环境变量与内置默认值。

```json
{
  "server": {
    "data_dir": "/var/lib/molink-server",
    "registration_token": "",
    "public_url": "",
    "offline_after_seconds": 90
  },
  "tls": {
    "enabled": true,
    "cert": "cer/server.crt", "key": "cer/server.key",
    "auto_self_signed": true
  },
  "device": {
    "host": "0.0.0.0", "port": 28430,
    "tls_cert": "", "tls_key": ""
  },
  "admin": {
    "host": "127.0.0.1", "port": 28431,
    "tls_cert": "", "tls_key": "",
    "web_root": "/opt/molink-server/share/web-admin",
    "allow_insecure_remote": false
  },
  "database": {
    "driver": "sqlite", "url": "",
    "pool_min": 1, "pool_max": 8, "busy_timeout_ms": 5000
  },
  "log": {
    "level": "info", "file": "",
    "access_log": false, "max_size_mb": 20, "max_files": 5
  }
}
```

常用命令行参数：

```
--config <file>  --host/--port  --no-tls  --tls-cert/--tls-key
--admin-host/--admin-port  --admin-tls-cert/--admin-tls-key
--ui-dir <dir>  --data-dir <dir>
--db-driver sqlite|postgres  --db-url <url>  --db-pool-min/--db-pool-max
--registration-token <t>  --public-url <url>  --offline-after <seconds>
--setup-token <t>  --allow-insecure-admin
--log-level debug|info|warn|error  --log-file <path>
--print-ready
```

环境变量：`MOLINK_SERVER_DATA_DIR`、`MOLINK_SERVER_CONFIG`、`MOLINK_SERVER_HOST`、
`MOLINK_SERVER_PORT`、`MOLINK_SERVER_ADMIN_HOST`、`MOLINK_SERVER_ADMIN_PORT`、
`MOLINK_SERVER_REGISTRATION_TOKEN`、`MOLINK_SERVER_PUBLIC_URL`、
`MOLINK_SERVER_DB_DRIVER`、`MOLINK_SERVER_DB_URL`、`MOLINK_SERVER_DB_POOL_MIN`、
`MOLINK_SERVER_DB_POOL_MAX`、`MOLINK_SERVER_DB_BUSY_TIMEOUT_MS`、
`MOLINK_SERVER_LOG_LEVEL`、`MOLINK_SERVER_LOG_FILE`、`MOLINK_SERVER_UI_DIR`、
`MOLINK_SERVER_SETUP_TOKEN`、`MOLINK_SERVER_ADMIN_ALLOW_INSECURE`、`MOLINK_SERVER_TLS`（`1`/`0`）、
`MOLINK_SERVER_TLS_CERT`、`MOLINK_SERVER_TLS_KEY`、
`MOLINK_SERVER_ADMIN_TLS_CERT`、`MOLINK_SERVER_ADMIN_TLS_KEY`。

管理页里的「安全策略」和「服务目录」会写回数据库并即时生效；监听地址、端口、
数据库连接和 UI 目录属于启动配置，修改后需要重启服务。TLS 可在管理页
「设置 → HTTPS 证书」修改（写回配置文件），同样需要重启。

## 5. 数据库

### SQLite（默认）

`database.driver = "sqlite"`，`url` 留空时使用 `<data_dir>/molink.db`。服务端使用
WAL、`busy_timeout` 和连接池；数据库必须放在本地磁盘（不要在 NFS/SMB 上运行
WAL）。备份：

```sh
sqlite3 /var/lib/molink-server/molink.db ".backup '/var/backups/molink-$(date +%F).db'"
```

### PostgreSQL（可选）

```json
"database": {
  "driver": "postgres",
  "url": "postgres://molink:secret@127.0.0.1:5432/molink",
  "pool_min": 1,
  "pool_max": 8,
  "busy_timeout_ms": 5000
}
```

- 目标机器需要安装 PostgreSQL 客户端库（`libpq`）；服务端通过运行时加载使用它，
  没有安装时只有 PostgreSQL 连接会失败，SQLite 不受影响。
- 可用 `MOLINK_SERVER_DB_URL` 把连接串放进 systemd 的 `EnvironmentFile`，避免
  把密码写进配置文件。
- 表结构与迁移由服务端在启动时自动创建；升级前请先 `pg_dump`。
- 连接串中的密码不会写入日志或管理 API。

## 6. 安全默认值

| 项目 | 默认 |
|---|---|
| IP 来源 | 私有网段 + 中国大陆（APNIC IPv4 表）；拒绝海外 |
| 自定义拒绝/允许 | 空；拒绝优先于允许 |
| 设备 API 限速 | 600 次/分钟/IP，突发 120 |
| 注册 | 10 次/小时/IP，突发 3；每 IP 最多 50 台设备 |
| 心跳 | 6 次/分钟/设备；240 次/分钟/IP |
| 查询 | 60 次/分钟/IP |
| 管理 API | 300 次/分钟/会话 |
| 登录失败 | 5 次触发阶梯 300/1800/21600/86400 秒，自动临时封禁 |
| 管理会话 | 空闲 30 分钟，绝对 12 小时；HttpOnly + SameSite=Strict |
| 密码 | PBKDF2-HMAC-SHA256，600000 次，16 字节盐 |

IPv6：私有/回环地址按私有处理，公网 IPv6 归入“海外”；v1 不内置中国 IPv6 表。

### 反向代理

管理面放在 nginx/Caddy 后时：

1. 管理面绑 `127.0.0.1`，由代理转发到本地端口。
2. 在「安全策略 → 受信代理」加入代理地址 CIDR，否则 `X-Forwarded-For` 不会被采用。
3. 若代理终止 TLS，把 `secure_cookie` 设为 `auto` 或 `always`，并保留
   `trust_forwarded_proto` 开启，以便 Secure Cookie 与 Origin 校验正确。

内置 TLS 默认开启，因此代理的上游是 `https://`。示例（nginx，自签名上游）：

```nginx
location / {
    proxy_pass https://127.0.0.1:28431;
    proxy_ssl_verify off;   # 上游是自签名证书；换成正式证书后可开启校验
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

如果希望代理终止 TLS、上游走明文 HTTP，先在配置文件中关闭内置 TLS
（`"tls": { "enabled": false }`）并重启，再把 `proxy_pass` 改回
`http://127.0.0.1:28431`。

设备 API 也可以按同样方式放在代理后；客户端填写代理的 `https://` 地址即可。

## 7. TLS

**默认启用 HTTPS。** 设备面与管理面共用一套证书，使用 PEM；私钥不能带密码
短语。证书必须覆盖客户端连接使用的域名（内置默认目录为 `molink.ssvr.top`），
并建议放入 **完整链**（站点证书 + 中间证书）。客户端会严格校验证书与主机名，
自签或私有 CA 需要在客户端额外提供 CA。

### 7.0 默认证书与自动生成

配置里的相对路径以 **`config.json` 所在目录**为基准（没有配置文件时用当前工作
目录）。默认值：

```json
"tls": {
  "enabled": true,
  "cert": "cer/server.crt",
  "key": "cer/server.key",
  "auto_self_signed": true
}
```

首次启动时如果这对文件都不存在，服务会自动生成一份 RSA 2048 自签名证书：

- 专有名称（CN）使用本机主机名；
- SAN 包含主机名、`localhost`、`127.0.0.1`、`::1` 和本机各网卡地址；
- 有效期 10 年，`cer/` 目录与私钥仅服务账户可读写。

自签名证书只适合内网或首次部署。客户端连接时需要把 `cer/server.crt` 填进
「设置 → 服务端 → 自定义 CA」。

管理页「设置 → HTTPS 证书」可以查看证书主题、有效期、SAN 与 SHA-256 指纹，
并执行：

| 操作 | 效果 |
|---|---|
| 关闭证书 | 写入 `"enabled": false`，重启后两个监听改为 HTTP |
| 上传证书 | 校验证书与私钥匹配后覆盖 `cer/server.crt` / `cer/server.key` |
| 重新生成自签名 | 用当前主机名与地址重新生成一对自签名证书 |

修改写入 `<data_dir>/tls.json`（不改动主配置文件），**重启 `molink-server` 后生效**（管理页会显示“重启后
生效”）。启用或关闭 HTTPS 后，管理页地址会在 `http://` 与 `https://` 之间
切换。

旧约定仍然可用：`<config.json 所在目录>/tls/fullchain.pem` + `privkey.pem`
（也接受 `cert.pem` + `key.pem`）在 `cer/` 不存在时会被自动发现；
`<data_dir>/tls/` 同样适用。

### 7.1 使用自己的证书

把证书与私钥放到任意路径，然后在配置文件中修改：

```json
{
  "tls": {
    "enabled": true,
    "cert": "/etc/molink-server/device.crt",
    "key": "/etc/molink-server/device.key",
    "auto_self_signed": false
  }
}
```

也可以直接在管理页上传，文件会覆盖到配置的 `cert` / `key` 路径。

```sh
sudo systemctl restart molink-server
sudo systemctl status molink-server
```

文件权限（Linux）：

```sh
sudo install -m 0644 -o root -g molink-server device.crt /etc/molink-server/
sudo install -m 0640 -o root -g molink-server device.key /etc/molink-server/
```

macOS 的服务账户是 `_molink`，Windows 服务以服务账户运行；确保对应账户能读取
私钥即可。私钥不要放在仓库或 Web 目录中。

系统包模板把证书放在数据目录下（Linux `/var/lib/molink-server/cer`、
macOS `/usr/local/var/molink-server/cer`、Windows
`C:\ProgramData\moLink\Server\cer`），确保服务账户可写。

### 7.2 命令行

```sh
# 关闭 TLS（两个监听都走 HTTP）
molink-server --no-tls

# 单独覆盖设备面证书（管理面继续使用共享证书）
molink-server --tls-cert /etc/molink-server/device.crt \
              --tls-key  /etc/molink-server/device.key

# 管理面单独覆盖
molink-server --admin-tls-cert /etc/molink-server/admin.crt \
              --admin-tls-key  /etc/molink-server/admin.key
```

### 7.3 验证

```sh
curl -sS https://molink.ssvr.top:28430/healthz
openssl s_client -connect molink.ssvr.top:28430 \
  -servername molink.ssvr.top </dev/null 2>/dev/null |
  openssl x509 -noout -subject -issuer -dates
```

`/healthz` 应返回 200；证书的 SAN 必须包含 `molink.ssvr.top`。主机名不匹配时
客户端会直接拒绝连接。

### 7.4 客户端

- 桌面客户端 0.8.1 起默认使用 `https://molink.ssvr.top:28430`；macOS 0.8.1
  的 dmg 已随包携带 OpenSSL，三端均可直接使用 HTTPS。
- 自签或私有 CA：在客户端「设置 → 服务端 → 自定义 CA」填写 CA 的 PEM 路径
  （`server_ca`），不要关闭校验。
- 开启 TLS 后，仍配置为 `http://` 的旧客户端会连接失败，需要同步改为 HTTPS。

### 7.5 注意

- 设备面 TLS 上只宣告 HTTP/1.1；管理面同样如此。
- 证书续期后重启服务即可生效。
- 目录 TLS 只保护注册、心跳、域组和查询；远程会话仍是设备直连，WebSocket
  当前为明文，需要部署方另行保护。

管理面绑定非回环且未启用 TLS 时启动会告警；显式允许需设置
`allow_insecure_remote` 或 `--allow-insecure-admin`。

## 8. 权限与首次管理员

- 管理员的密码哈希保存在数据库；数据库文件权限由 systemd `StateDirectory`
  保证为 `molink-server` 用户所有。
- 不支持默认密码；没有用户时只能通过初始化表单或令牌创建第一个管理员。
- 忘记密码：在数据库里删除 `admin_users` 行会重新开放初始化（会丢失账户）；
  更安全的做法是备份数据库后使用 SQL 重置。请先阅读 `devices.json` 旧数据导入
  规则。

## 9. 健康检查与日志

- `GET /healthz`（设备面）与管理面 `/healthz` 返回服务版本与数据库状态。
- 管理页仪表盘显示运行时间、在线设备、数据库 schema 版本。
- 日志默认写 stderr（systemd 收集到 journald）；配置文件/安装器会把日志写入
  文件并按大小轮转（默认 20 MiB × 5）。

## 10. 升级

1. 备份数据库（SQLite 复制/`.backup`，PostgreSQL `pg_dump`）。
2. 停止服务，替换二进制与 `web-admin`，保持 `/etc` 与 `/var/lib` 不变。
3. 启动；服务端会在事务中执行新的 schema 迁移。
4. 检查 `/healthz` 与管理页概览。

旧版（0.1.x）的 `devices.json` 目录会在首次启动时导入到数据库；导入后文件会
改名为 `devices.json.imported.<时间戳>`，保留原文件以便回退。

0.2.1 的迁移（schema 第 5 版）为设备表增加 `platform`、`avatar`、`domain_code`，
并新建 `domains` 表。已有设备升级后 `domain_code` 为空，下一次心跳只会上报元数据，
不会自动分码；需要域组时在客户端点一次「更新域组码」即可。设备 ID、token 与
注册记录不受影响，无需重新注册。

回退到旧版本前请先恢复对应数据库备份：旧二进制不认识新的列与 `domains` 表。

## 11. 故障排查

| 现象 | 处理 |
|---|---|
| 设备注册返回 403 `ip_denied` | 来源不在私有/中国/自定义允许范围；调整安全策略或加代理 |
| 403 `ip_banned` | 在「封禁」中解除，或等待自动封禁到期 |
| 429 `rate_limited` | 查看响应 `Retry-After`；检查安全策略中的限速 |
| 登录 403 `csrf_failed` | 浏览器时间/代理导致 Cookie 与请求头不一致；刷新页面重新登录 |
| 管理页 401 | 会话过期或服务重启；重新登录 |
| 初始化需要令牌 | 查看服务启动日志中的一次性 token，或使用 `--setup-token` |
| Windows 服务起不来 | 查看 `C:\ProgramData\moLink\Server\server.log`；确认配置文件路径 |
| PostgreSQL `NO_DRIVER` | 目标机器缺少 libpq；安装客户端库或改用 SQLite |
| macOS pkg 被拦截 | 右键打开 / 去除 quarantine；正式签名后续提供 |

## 12. 规模与限制

- 设计目标为单机自托管、约一万台设备；SQLite 足够；更大规模建议 PostgreSQL。
- 目录只负责寻址，远程会话仍需要对方验证码或人工批准；目前没有中继。
- 地址查询按 9 位设备 ID 公开；设备 ID 应按凭据对待。
- 域组列表单次最多返回 100 台（不含调用者）；码只能由服务端生成，撤销后不可恢复。
- 同一 IP 最多注册 50 台设备；注册、心跳、查询与域组操作均有限速，见
  [device-api.md §限速](device-api.md#限速)。
- 分布式/多节点管理属于第三阶段（Pro），见
  [server-distributed.md](server-distributed.md)。
