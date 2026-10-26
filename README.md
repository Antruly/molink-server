<p align="center">
  <img src="packaging/icons/molink-server.svg" width="96" alt="moLink Server icon">
</p>

# moLink Server

moLink 的设备目录与自托管管理服务：客户端在这里注册与心跳，按设备 ID 或域组
（频道）互相发现，然后建立直连；服务端同时提供 Web 管理控制台、账号与安全策略。

当前版本 **0.3.4**。本仓库独立于桌面客户端
（[Antruly/molink](https://github.com/Antruly/molink)），使用自己的 `v*` 标签
发布，服务端与客户端不再共用版本线。

- 部署与运维：[docs/server.md](docs/server.md)
- 设备 API 参考：[docs/device-api.md](docs/device-api.md)
- 第三阶段分布式设计：[docs/server-distributed.md](docs/server-distributed.md)
- 变更记录：[CHANGELOG.md](CHANGELOG.md)

## 功能

- **设备目录**：注册、30 秒心跳、在线状态、按 ID 查询地址、注销；支持
  `ids` 批量查询。目录只做寻址，媒体仍走客户端直连。
- **域组码（频道）**：注册时由服务端签发唯一 9 位码，可复制给其他设备加入；
  同组设备可互相列出，码的所有者更新后旧码立即失效。
- **管理控制台**：首次初始化、仪表盘、设备、管理员、安全策略、封禁、审计、
  设置；亮/暗主题。
- **账号与权限**：admin / operator / viewer；PBKDF2-HMAC-SHA256（600000 轮）、
  HttpOnly 会话 + CSRF、登录失败阶梯。
- **安全策略**：私有 / 中国大陆 / 海外 + 自定义 CIDR；请求限速、临时封禁、
  可信代理 `X-Forwarded-For`、审计日志。
- **存储**：默认 SQLite（WAL、连接池、事务化迁移），可选 PostgreSQL。
- **传输**：设备面与管理面分端口；可选原生 TLS。

## 快速开始

### 预编译安装包

从 [Releases](https://github.com/Antruly/molink-server/releases) 下载：

| 平台 | 资产 |
|---|---|
| Linux | `.deb` / `.rpm`（自动注册 systemd）、`.tar.gz`（`install.sh`） |
| Windows | Inno Setup 安装器（可选安装为服务）、便携 `.zip` |
| macOS | 未签名 `.pkg`（LaunchDaemon）、`.tar.gz` |

```sh
# Linux（版本号以 Releases 页面为准）
sudo dpkg -i molink-server_<version>_amd64.deb
systemctl status molink-server
journalctl -u molink-server -f
```

安装路径、Windows 服务名、macOS launchd 与卸载方式见
[docs/server.md §2](docs/server.md#2-安装)。

### 从源码构建

依赖：CMake ≥ 3.20、C++20 编译器、OpenSSL、libuv、zlib、Node.js LTS + pnpm。
服务端需要带 **DB 模块**的 libuvcpp（SQLite 必需，PostgreSQL 可选）：

```sh
cmake -S <libuvcpp> -B <libuvcpp-build> \
  -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_DB_SQLITE_FROM_SOURCE=ON \
  -DUVCPP_ENABLE_DB_PGSQL=ON -DUVCPP_DB_RUNTIME_LOAD=ON \
  -DUVCPP_ENABLE_DB_MYSQL=OFF -DUVCPP_ENABLE_DB_ODBC=OFF \
  -DUVCPP_ENABLE_DB_MONGODB=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/uvcpp"
cmake --build <libuvcpp-build> --parallel
cmake --install <libuvcpp-build>

# 服务端与测试
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/uvcpp" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 管理控制台（静态文件输出到 ui/dist）
cd ui && pnpm install --frozen-lockfile && pnpm build
```

### 运行

```sh
./build/molink-server --data-dir ./molink-server-data
# 首次启动会在 ./cer/ 下自动生成自签名证书
# 管理控制台：https://127.0.0.1:28431 （浏览器会提示自签名证书）
# 设备 API：  https://127.0.0.1:28430
# 中继：      quic://127.0.0.1:28442（TLS） / tcp://127.0.0.1:28441（TLS）
#            https://127.0.0.1:28444（HTTP/3 WebSocket；0.3.4 起暂时关闭，见 docs/p2p-relay.md）
#            udp://127.0.0.1:28443（无连接，丢包由上层修复）
```

默认启用 HTTPS。需要纯 HTTP 时加 `--no-tls`，或在管理页「设置 → HTTPS 证书」
关闭后重启服务。

首次打开管理页会要求创建管理员（至少 10 位密码）。如果管理面绑定在非回环
地址，启动日志会打印一次性初始化令牌，也可用 `--setup-token` 固定。
`--help` 列出全部参数；管理面找不到 `ui/dist` 时可用 `--ui-dir` 指定。

## 配置

优先级：**命令行 > 环境变量 > 配置文件 > 内置默认值**。最小配置：

```json
{
  "server": { "data_dir": "/var/lib/molink-server" },
  "tls": {
    "enabled": true,
    "cert": "cer/server.crt",
    "key": "cer/server.key",
    "auto_self_signed": true
  },
  "device": { "host": "0.0.0.0", "port": 28430 },
  "admin":  { "host": "127.0.0.1", "port": 28431 },
  "relay":  { "tcp_port": 28441, "quic_port": 28442, "udp_port": 28443,
              "http3_port": 28444 }
}
```

```sh
./molink-server --config /etc/molink-server/config.json
```

完整的 `config.json` 字段、环境变量、TLS、反向代理、数据库与日志选项见
[docs/server.md §4-§7](docs/server.md#4-配置)。管理页中的安全策略与服务目录
写回数据库并即时生效；监听地址、端口、TLS 与数据库连接需要重启。

未指定 `--config` / `MOLINK_SERVER_CONFIG` 时，服务会读取当前目录的
`config.json`。安装目录（或 `config.json` 所在目录）会附带一份
`config.example.json`，其中的证书与私钥默认指向同目录下的 `cer/`：

```text
<config.json 所在目录>/cer/server.crt
<config.json 所在目录>/cer/server.key
```

首次启动时如果这对文件不存在，服务会自动生成一份覆盖本机名、`localhost` 与
本机 IP 的自签名证书（`auto_self_signed: true`），随后两个监听都使用 HTTPS。
客户端连接自签名目录时，需要把 `cer/server.crt` 导入为自定义 CA。

管理页「设置 → HTTPS 证书」可以：

- 关闭证书（改用 HTTP）或重新启用；
- 上传自己的证书与私钥（PEM，私钥不能带口令）；
- 重新生成自签名证书。

修改写入 `<data_dir>/tls.json`（不改动主配置文件），重启 `molink-server` 后生效。

兼容旧约定：`<config.json 所在目录>/tls/fullchain.pem` + `privkey.pem`
（也接受 `cert.pem` + `key.pem`）在 `cer/` 不存在时仍会被自动发现；也可用
`MOLINK_SERVER_TLS_CERT` / `MOLINK_SERVER_TLS_KEY` 环境变量做单监听覆盖。

## 设备 API

| 方法 | 路径 | 说明 | 鉴权 |
|---|---|---|---|
| `POST` | `/v1/devices/register` | 注册，返回设备 ID、token 与域组码 | 可选邀请码 |
| `POST` | `/v1/devices/heartbeat` | 心跳与元数据更新 | id + token |
| `POST` | `/v1/devices/domain` | 加入 / 更新 / 退出域组 | id + token |
| `GET` | `/v1/devices?id=` / `?ids=` | 按 ID 查询地址 | 无 |
| `GET` | `/v1/devices/domain` | 列出同域组设备 | id + token |
| `POST` | `/v1/devices/unregister` | 注销设备 | id + token |
| `POST` | `/v1/connect` | 发起 P2P 呼叫（ICE offer + 候选地址） | id + token |
| `GET` | `/v1/connect/pending` | 被叫方轮询待处理的呼叫 | id + token |
| `POST` | `/v1/connect/answer` | 被叫方接受呼叫（answer + 候选地址） | id + token |
| `GET` | `/v1/connect/status` | 查询呼叫状态与对方候选地址 | id + token |
| `POST` | `/v1/connect/candidates` | 追加 trickle 候选地址 | id + token |
| `POST` | `/v1/connect/close` | 关闭呼叫 | id + token |
| `GET` | `/healthz` | 数据库健康检查 | 无 |

P2P 打洞、中继与 QUIC 数据面的设计见
[docs/p2p-relay.md](docs/p2p-relay.md)。

```sh
curl -s -X POST http://127.0.0.1:28430/v1/devices/register \
  -H 'Content-Type: application/json' \
  -d '{"name":"Office PC","port":28418,"version":"0.8.1","platform":"windows"}'
```

```json
{"device_id":"NM8698LBD","display_id":"NM8-698-LBD","token":"...",
 "domain":"E8XRGU7GF","domain_valid":true,"server_time":1792869596040}
```

字段、错误码、限速、域组语义与完整示例见
[docs/device-api.md](docs/device-api.md)。

## 域组码

每台设备注册后由服务端签发一个 9 位码（去掉易混淆的 `I/1/O/0`），客户端不能
自行生成：

- 把码发给同事，对方通过 `join` 进入同一频道；`GET /v1/devices/domain` 只返回
  同码设备。
- 码的所有者执行 `rotate` 会撤销旧码：旧成员下一次心跳收到
  `domain_valid=false`，列表查询返回 `unknown_domain`。
- 域组只用于发现与筛选，实际连接仍需要对方验证码或访问确认。

## 管理控制台

`https://127.0.0.1:28431`（首次启动会生成自签名证书），创建管理员。角色：

| 角色 | 权限 |
|---|---|
| admin | 全部，包括账户、安全策略与服务设置 |
| operator | 查看、吊销设备、管理封禁、查看审计 |
| viewer | 只读 |

管理面默认只绑定回环地址；公网部署请保持回环 + SSH 隧道，或配置 TLS /
反向代理，见 [docs/server.md §6](docs/server.md#6-安全默认值)。

## 数据与备份

- 默认 `data_dir = ./molink-server-data`，SQLite 数据库为 `<data_dir>/molink.db`。
- 包安装的数据目录：Linux `/var/lib/molink-server`、Windows
  `C:\ProgramData\moLink\Server`、macOS `/usr/local/var/molink-server`。
- SQLite 使用 WAL，必须放在本地磁盘；备份：

```sh
sqlite3 /var/lib/molink-server/molink.db ".backup '/var/backups/molink-$(date +%F).db'"
```

- PostgreSQL 连接串可用 `MOLINK_SERVER_DB_URL` 注入；升级前先 `pg_dump`。
- 升级时服务端在事务中自动执行 schema 迁移（当前为第 5 版）。

## 仓库结构

```text
molink-server/
├── src/                 设备 API、管理 API、数据库、安全策略
├── tests/               foundation / security / repository / auth
├── ui/                  管理控制台（React + Vite）
├── third_party/         PostgreSQL 客户端头文件
├── tools/               CN CIDR 生成脚本
├── packaging/           linux · macos · windows 安装包脚本
├── docs/                部署、设备 API、分布式设计
└── .github/workflows/   发布工作流（v* 标签）
```

## 开发与测试

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/.local/uvcpp" -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure

cd ui
pnpm install
pnpm dev        # 开发服务器（默认对接 127.0.0.1:28431）
pnpm build
```

提交前请至少跑一遍 `ctest` 与 `ui` 的构建。设备 API 的回归覆盖在
`tests/repository_test.cpp`（含域组 join/rotate/失效流程）。

## 打包与发布

推送 `v*` 标签（例如 `v0.3.4`）触发
[`.github/workflows/release.yml`](.github/workflows/release.yml)，在
Linux / Windows / macOS 上并行构建并上传安装包。CI 需要的仓库变量：

| 变量 | 说明 |
|---|---|
| `UVCPP_VERSION` | libuvcpp 预编译包版本（Linux/Windows，默认 `1.8.6`） |
| `UVCPP_REPO` | libuvcpp 仓库地址（macOS 源码构建使用） |
| `UVCPP_REF` | 分支或标签（macOS 默认 `master`） |

本地打包：

```sh
./packaging/linux/build-server-deb.sh
./packaging/linux/build-server-rpm.sh
./packaging/linux/build-server-tar.sh
./packaging/macos/build-server-pkg.sh
./packaging/macos/build-server-tar.sh
# Windows：packaging/windows/server-stage.sh + server-installer.iss
```

产物输出到 `build/package/`。历史 0.2.0 发布保留了旧的 `server-v0.2.0` 标签名，
新版本统一使用 `v*`。

## 兼容性

- 桌面客户端 0.8.x 可直接使用；注册与心跳同时接受 JSON 请求体与查询参数。
- 0.2.1 新增设备元数据、域组码与批量查询；旧客户端会忽略新增字段。
- 0.1.x 的 `devices.json` 目录会在首次启动时自动导入，原文件保留。

## 许可证

MIT，见 [LICENSE](LICENSE)。
