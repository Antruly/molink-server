import { useCallback, useEffect, useState } from 'react'
import { Copy, RefreshCw, Save, Server, ShieldCheck, Upload } from 'lucide-react'
import {
  ApiError,
  api,
  type Overview,
  type ServerSettings,
  type TlsStatus,
} from '../lib/api'
import { roleRank, type Notify, type Role } from '../lib/types'
import { formatUptime } from '../lib/format'
import {
  Badge,
  Card,
  ErrorBanner,
  Field,
  PageHeader,
  Spinner,
} from '../components/ui'

export function SettingsPage({ role, notify }: { role: Role; notify: Notify }) {
  const [value, setValue] = useState<ServerSettings | null>(null)
  const [overview, setOverview] = useState<Overview | null>(null)
  const [tls, setTls] = useState<TlsStatus | null>(null)
  const [certPem, setCertPem] = useState('')
  const [keyPem, setKeyPem] = useState('')
  const [certName, setCertName] = useState('')
  const [keyName, setKeyName] = useState('')
  const [tlsBusy, setTlsBusy] = useState(false)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const canEdit = roleRank(role) >= 3

  const load = useCallback(async () => {
    setLoading(true)
    setError(null)
    try {
      const [settings, info, tlsStatus] = await Promise.all([
        api.serverSettings(),
        api.overview(),
        api.tls(),
      ])
      setValue(settings)
      setOverview(info)
      setTls(tlsStatus)
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '无法加载设置')
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const save = async () => {
    if (!value) return
    setBusy(true)
    setError(null)
    try {
      const saved = await api.saveServerSettings(value)
      setValue(saved)
      notify('服务设置已保存')
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : '保存失败')
    } finally {
      setBusy(false)
    }
  }

  const applyTls = async (run: () => Promise<TlsStatus>, message: string) => {
    setTlsBusy(true)
    setError(null)
    try {
      setTls(await run())
      notify(message)
    } catch (caught) {
      setError(caught instanceof ApiError ? caught.message : 'TLS 设置失败')
    } finally {
      setTlsBusy(false)
    }
  }

  const toggleTls = () => {
    if (!tls) return
    void applyTls(
      () => api.setTlsEnabled(!tls.enabled),
      !tls.enabled ? '已启用 HTTPS，重启服务后生效' : '已停用 HTTPS，重启服务后生效',
    )
  }

  const uploadCertificate = () => {
    if (!certPem || !keyPem) return
    void applyTls(
      () => api.uploadTlsCertificate(certPem, keyPem),
      '证书已保存，重启服务后生效',
    )
  }

  const regenerate = () => {
    if (!window.confirm('将覆盖当前证书并重新生成自签名证书，继续？')) return
    void applyTls(
      () => api.resetSelfSigned(),
      '已生成自签名证书，重启服务后生效',
    )
  }

  const readFile = (file: File, target: 'cert' | 'key') => {
    void file.text().then((text) => {
      if (target === 'cert') {
        setCertPem(text)
        setCertName(file.name)
      } else {
        setKeyPem(text)
        setKeyName(file.name)
      }
    })
  }

  if (loading) return <Spinner />
  if (!value) return <ErrorBanner message={error ?? '无法加载设置'} />

  return (
    <div>
      <PageHeader
        title="设置"
        description="注册、离线判定与公开地址；监听地址与数据库在服务端配置文件中设置。"
        actions={
          <button
            type="button"
            className="btn btn-primary h-8 gap-1 px-2 text-[11px]"
            onClick={() => void save()}
            disabled={!canEdit || busy}
          >
            <Save size={13} />
            {busy ? '保存中…' : '保存设置'}
          </button>
        }
      />
      <ErrorBanner message={error} />

      <div className="mt-3 grid gap-4 xl:grid-cols-2">
        <Card>
          <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
            <Server size={15} className="text-accent" />
            服务目录
          </div>
          <div className="space-y-3">
            <Field label="服务名称">
              <input
                className="field"
                value={value.name}
                disabled={!canEdit}
                onChange={(event) => setValue({ ...value, name: event.target.value })}
              />
            </Field>
            <Field label="公开地址" hint="客户端通过设备 ID 连接时展示/解析的地址；留空表示不公开">
              <input
                className="field font-mono text-[11px]"
                value={value.public_url}
                disabled={!canEdit}
                placeholder="https://molink.example.com"
                onChange={(event) => setValue({ ...value, public_url: event.target.value })}
              />
            </Field>
            <Field
              label="注册邀请码"
              hint="留空表示开放注册；设置后客户端注册必须携带同一值"
            >
              <div className="flex gap-2">
                <input
                  className="field font-mono text-[11px]"
                  value={value.registration_token}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({ ...value, registration_token: event.target.value })
                  }
                />
                <button
                  type="button"
                  className="btn h-9 w-9 shrink-0 !px-0"
                  title="复制"
                  disabled={!value.registration_token}
                  onClick={() => {
                    void navigator.clipboard?.writeText(value.registration_token)
                    notify('邀请码已复制')
                  }}
                >
                  <Copy size={14} />
                </button>
              </div>
            </Field>
            <div className="grid grid-cols-2 gap-3">
              <Field label="离线判定（秒）">
                <input
                  className="field h-9"
                  type="number"
                  min={1}
                  value={value.offline_after_seconds}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({
                      ...value,
                      offline_after_seconds: Math.max(1, Number(event.target.value) || 90),
                    })
                  }
                />
              </Field>
              <Field label="设备上限">
                <input
                  className="field h-9"
                  type="number"
                  min={1}
                  value={value.max_devices}
                  disabled={!canEdit}
                  onChange={(event) =>
                    setValue({
                      ...value,
                      max_devices: Math.max(1, Number(event.target.value) || 10000),
                    })
                  }
                />
              </Field>
            </div>
          </div>
        </Card>

        <Card>
          <div className="mb-3 flex items-center gap-2 text-[13px] font-medium text-ink">
            <Server size={15} className="text-accent" />
            运行信息
          </div>
          {overview ? (
            <dl className="space-y-2 text-[12px]">
              <Row label="版本" value={overview.version} mono />
              <Row label="运行时间" value={formatUptime(overview.uptime_ms)} />
              <Row
                label="设备 API"
                value={`${overview.device_api.host}:${overview.device_api.port}`}
                mono
              />
              <Row
                label="管理 API"
                value={`${overview.admin_api.host}:${overview.admin_api.port}`}
                mono
              />
              <Row
                label="数据库"
                value={`${overview.database.driver} · schema ${overview.database.schema_version}`}
                mono
              />
              <Row label="设备" value={`${overview.devices.total} 台 · ${overview.devices.online} 在线`} />
            </dl>
          ) : null}
          <div className="mt-4 rounded-xl border border-line-soft bg-panel/50 p-3 text-[10px] leading-relaxed text-muted">
            <div className="mb-1 flex items-center gap-1 text-[11px] text-ink">
              <Badge tone="warn">需要重启</Badge>
              监听地址、端口、数据库连接与 UI 目录来自服务端配置文件/命令行；
              修改后需重启服务。TLS 证书可在下方修改。
            </div>
          </div>
        </Card>

        <Card className="xl:col-span-2">
          <div className="mb-3 flex flex-wrap items-center gap-2 text-[13px] font-medium text-ink">
            <ShieldCheck size={15} className="text-accent" />
            HTTPS 证书
            {tls?.restart_required ? <Badge tone="warn">重启后生效</Badge> : null}
          </div>
          {tls ? (
            <>
              <div className="flex flex-wrap items-center justify-between gap-3 rounded-xl border border-line-soft bg-panel/40 px-3 py-2">
                <div className="min-w-0">
                  <div className="text-[12px] text-ink">
                    {tls.enabled ? '已启用 HTTPS' : 'HTTPS 已关闭（HTTP）'}
                  </div>
                  <div className="text-[10px] leading-relaxed text-muted">
                    {tls.running_enabled !== tls.enabled
                      ? `当前进程仍在${tls.running_enabled ? '使用 HTTPS' : '使用 HTTP'}，重启服务后切换为${tls.enabled ? ' HTTPS' : ' HTTP'}。`
                      : '设备 API 与管理页共用同一套证书；客户端连接自签名服务时需导入该证书作为 CA。'}
                  </div>
                </div>
                <button
                  type="button"
                  className="btn h-8 shrink-0 px-3 text-[11px]"
                  onClick={toggleTls}
                  disabled={!canEdit || tlsBusy}
                >
                  {tls.enabled ? '关闭证书 / 改用 HTTP' : '启用证书 / 改用 HTTPS'}
                </button>
              </div>

              <dl className="mt-3 grid gap-x-6 gap-y-2 text-[12px] sm:grid-cols-2">
                <Row
                  label="来源"
                  value={
                    tls.certificate.found
                      ? tls.certificate.self_signed
                        ? '自签名'
                        : '自定义证书'
                      : '未找到'
                  }
                />
                <Row label="主题" value={tls.certificate.subject || '—'} mono />
                <Row label="颁发者" value={tls.certificate.issuer || '—'} mono />
                <Row
                  label="有效期"
                  value={
                    tls.certificate.not_after_ms
                      ? `至 ${new Date(tls.certificate.not_after_ms).toLocaleDateString()}`
                      : '—'
                  }
                />
                <Row label="证书文件" value={tls.cert || '—'} mono />
                <Row label="私钥文件" value={tls.key || '—'} mono />
              </dl>
              <div className="mt-2 break-all text-[10px] leading-relaxed text-muted">
                域名 / IP：
                {[...tls.certificate.dns_names, ...tls.certificate.ip_addresses].join(', ') ||
                  '—'}
              </div>
              <div className="break-all text-[10px] leading-relaxed text-muted">
                SHA-256：{tls.certificate.fingerprint_sha256 || '—'}
              </div>

              {canEdit ? (
                <div className="mt-4 grid gap-3 lg:grid-cols-2">
                  <Field label="上传证书（PEM，可含完整链）">
                    <input
                      type="file"
                      accept=".pem,.crt,.cer,.txt"
                      className="field h-9 py-1 text-[11px]"
                      disabled={tlsBusy}
                      onChange={(event) => {
                        const file = event.target.files?.[0]
                        if (file) readFile(file, 'cert')
                      }}
                    />
                    {certName ? (
                      <span className="mt-1 block text-[10px] text-muted">
                        {certName}
                      </span>
                    ) : null}
                  </Field>
                  <Field label="上传私钥（PEM，无口令）">
                    <input
                      type="file"
                      accept=".pem,.key,.txt"
                      className="field h-9 py-1 text-[11px]"
                      disabled={tlsBusy}
                      onChange={(event) => {
                        const file = event.target.files?.[0]
                        if (file) readFile(file, 'key')
                      }}
                    />
                    {keyName ? (
                      <span className="mt-1 block text-[10px] text-muted">
                        {keyName}
                      </span>
                    ) : null}
                  </Field>
                  <div className="flex flex-wrap gap-2 lg:col-span-2">
                    <button
                      type="button"
                      className="btn btn-primary h-8 gap-1 px-3 text-[11px]"
                      onClick={uploadCertificate}
                      disabled={!certPem || !keyPem || tlsBusy}
                    >
                      <Upload size={13} />
                      上传并使用
                    </button>
                    <button
                      type="button"
                      className="btn h-8 gap-1 px-3 text-[11px]"
                      onClick={regenerate}
                      disabled={tlsBusy}
                    >
                      <RefreshCw size={13} />
                      重新生成自签名证书
                    </button>
                  </div>
                </div>
              ) : null}

              <div className="mt-3 rounded-xl border border-line-soft bg-panel/50 p-3 text-[10px] leading-relaxed text-muted">
                证书修改会写入 {tls.config_path}（不改动主配置文件），重启
                molink-server 后生效。启用或关闭 HTTPS 后，管理页地址会在 http:// 与
                https:// 之间切换；自签名证书需要浏览器或客户端手动信任。
              </div>
            </>
          ) : (
            <div className="text-[11px] text-muted">无法加载 TLS 状态。</div>
          )}
        </Card>
      </div>
    </div>
  )
}

function Row({ label, value, mono = false }: { label: string; value: string; mono?: boolean }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <dt className="text-muted">{label}</dt>
      <dd className={mono ? 'font-mono text-[11px] text-ink' : 'text-ink'}>{value}</dd>
    </div>
  )
}
