// =====================================================================
//  plugin_runtime.rs  -  NexOS 应用级插件运行时（Rust 宿主侧）
// ---------------------------------------------------------------------
//  与内核态插件加载器（plugins/loader/plugin_loader.cpp）分工：
//
//    内核 loader : 能力位、沙盒根、句柄代际、路径边界（不可绕过）
//    本模块      : 异步调度、超时/取消、panic 隔离、域名白名单、MCP 路由
//
//  四条铁律在本文件里的落点：
//    1. 一切皆插件    -> install() 只接受 PluginManifest，不存在“单体应用”
//    2. 单进程零 IPC  -> 插件是同一进程里的 Future，没有 socket/管道
//    3. MCP 协议      -> dispatch_jsonrpc() 实现 initialize / tools/list /
//                        tools/call / ping，报文是 JSON-RPC 2.0
//    4. 内核无 stdlib -> 本文件是宿主侧，可以用 std；内核侧不引用本文件
//
//  安全模型（为什么 abort 还不够）：
//    * tokio::spawn 的 abort 只在 .await 点生效。插件如果写 `loop {}`
//      永不 yield，abort 无法抢占它 —— 所以**插件 handler 必须是 async**，
//      长 CPU 计算要主动 tokio::task::yield_now().await。文档里明写了这条。
//    * catch_unwind 把 panic 变成 ToolResult{is_error:true}，任务不会
//      带着 panic 逃逸到宿主 runtime。
//    * 超时后先 abort 再 await JoinHandle：返回时任务已被 drop，不会在
//      调用方拿到 Err 之后继续改共享状态。
// =====================================================================

#[cfg(panic = "abort")]
compile_error!(
    "nexos-plugin-runtime must be built with panic=unwind: \
     catch_unwind() is a no-op under panic=abort and one plugin panic would kill the host"
);

use std::any::Any;
use std::collections::HashMap;
use std::fmt;
use std::future::Future;
use std::net::IpAddr;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::pin::Pin;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tokio::sync::{RwLock, Semaphore};
use tokio::task::JoinHandle;
use tokio::time::timeout;

/// MCP 协议版本（initialize 回包）。
pub const MCP_PROTOCOL_VERSION: &str = "2025-06-18";

// =====================================================================
//  1. ToolResult —— MCP tools/call 的返回体
// =====================================================================

/// MCP content block。只实现最常用的三种；未知类型在反序列化时失败，
/// 避免把任意 JSON 透传给前端。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ContentBlock {
    Text {
        text: String,
    },
    Image {
        /// base64（不含 data: 前缀）。
        data: String,
        #[serde(rename = "mimeType")]
        mime_type: String,
    },
    Resource {
        uri: String,
        #[serde(rename = "mimeType", skip_serializing_if = "Option::is_none")]
        mime_type: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        text: Option<String>,
    },
}

/// MCP 工具结果。
///
/// `structured_content` 放机器可读的 JSON（前端按 `ui_layout` 绑定），
/// `content` 放给 LLM/人类看的文本。二者可以同时存在。
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ToolResult {
    pub content: Vec<ContentBlock>,

    #[serde(
        rename = "structuredContent",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub structured_content: Option<Value>,

    #[serde(rename = "isError", default)]
    pub is_error: bool,
}

impl ToolResult {
    /// 纯文本成功结果。
    pub fn text(text: impl Into<String>) -> Self {
        Self {
            content: vec![ContentBlock::Text { text: text.into() }],
            structured_content: None,
            is_error: false,
        }
    }

    /// 结构化成功结果：文本给模型，JSON 给 UI。
    pub fn json(structured: Value) -> Self {
        let text = serde_json::to_string(&structured).unwrap_or_else(|_| "{}".to_string());
        Self {
            content: vec![ContentBlock::Text { text }],
            structured_content: Some(structured),
            is_error: false,
        }
    }

    /// 插件返回的业务错误（不是协议错误）。
    pub fn error(message: impl Into<String>) -> Self {
        Self {
            content: vec![ContentBlock::Text {
                text: message.into(),
            }],
            structured_content: None,
            is_error: true,
        }
    }

    pub fn with_structured(mut self, value: Value) -> Self {
        self.structured_content = Some(value);
        self
    }

    /// 转成 `result` 字段的内容（不含 jsonrpc/id 外壳）。
    pub fn to_result_json(&self) -> Value {
        json!({
            "content": self.content,
            "structuredContent": self.structured_content,
            "isError": self.is_error,
        })
    }
}

// =====================================================================
//  2. 能力位 / 清单 / 策略
// =====================================================================

/// 与内核 CAP_* 一一对应。内核是权威；这里只是镜像，用于在派发前
/// 再检查一次（纵深防御：即使内核被绕过，宿主也不会执行越权工具）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CapSet(u64);

impl CapSet {
    pub const EMPTY: CapSet = CapSet(0);
    pub const FS_READ: CapSet = CapSet(1 << 0);
    pub const FS_WRITE: CapSet = CapSet(1 << 1);
    pub const NET: CapSet = CapSet(1 << 2);
    pub const PROC: CapSet = CapSet(1 << 3);
    pub const UI: CapSet = CapSet(1 << 4);
    pub const AI: CapSet = CapSet(1 << 5);
    pub const DEV: CapSet = CapSet(1 << 6);
    pub const SYSINFO: CapSet = CapSet(1 << 7);

    pub const fn bits(self) -> u64 {
        self.0
    }

    pub const fn from_bits(bits: u64) -> Self {
        CapSet(bits)
    }

    pub const fn contains(self, other: CapSet) -> bool {
        (self.0 & other.0) == other.0
    }

    pub const fn union(self, other: CapSet) -> CapSet {
        CapSet(self.0 | other.0)
    }

    pub const fn strip(self, other: CapSet) -> CapSet {
        CapSet(self.0 & !other.0)
    }

    pub fn names(self) -> Vec<&'static str> {
        let table = [
            (CapSet::FS_READ, "fs.read"),
            (CapSet::FS_WRITE, "fs.write"),
            (CapSet::NET, "net"),
            (CapSet::PROC, "proc"),
            (CapSet::UI, "ui"),
            (CapSet::AI, "ai"),
            (CapSet::DEV, "dev"),
            (CapSet::SYSINFO, "sys.info"),
        ];
        table
            .iter()
            .filter(|(bit, _)| self.contains(*bit))
            .map(|(_, name)| *name)
            .collect()
    }
}

fn capability_from_name(name: &str) -> Option<CapSet> {
    match name {
        "fs.read" | "fs_read" => Some(CapSet::FS_READ),
        "fs.write" | "fs_write" => Some(CapSet::FS_WRITE),
        "net" | "net.http" | "net_http" => Some(CapSet::NET),
        "proc" | "sys.proc" => Some(CapSet::PROC),
        "ui" | "ui.patch" => Some(CapSet::UI),
        "ai" | "ai.infer" => Some(CapSet::AI),
        "dev" | "dev.ioctl" => Some(CapSet::DEV),
        "sys.info" | "sys_info" => Some(CapSet::SYSINFO),
        _ => None,
    }
}

/// MCP 工具需要的能力位。与内核 pl_tool_cap() 保持同步。
fn tool_requirement(tool: &str) -> Option<CapSet> {
    match tool {
        "fs_read" | "fs.read" | "fs_list" | "fs.list" | "fs_stat" | "fs.stat" => {
            Some(CapSet::FS_READ)
        }
        "fs_write" | "fs.write" | "fs_delete" | "fs.delete" | "fs_mkdir" | "fs.mkdir" => {
            Some(CapSet::FS_WRITE)
        }
        "net_http" | "net.http" | "net_fetch" | "net.fetch" | "net_dns" | "net.dns" => {
            Some(CapSet::NET)
        }
        "sys_info" | "sys.info" | "sys_tasks" | "sys.tasks" | "sys_mem" | "sys.mem" => {
            Some(CapSet::SYSINFO)
        }
        "ui_patch" | "ui.patch" | "ui_toast" | "ui.toast" => Some(CapSet::UI),
        "ai_infer" | "ai.infer" | "ai_embed" | "ai.embed" => Some(CapSet::AI),
        "dev_ioctl" | "dev.ioctl" => Some(CapSet::DEV),
        _ => None,
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct NetworkPolicy {
    /// 域名白名单。空数组 = 完全禁止出站网络。
    #[serde(default)]
    pub allow_hosts: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ToolSpec {
    pub name: String,
    #[serde(default)]
    pub description: String,
    #[serde(default, rename = "inputSchema")]
    pub input_schema: Value,
}

/// 整合包里的单个插件清单（bundle 的 plugins[] 元素 + 各插件目录下的
/// plugin.json 使用同一份 schema，见 docs/schemas/manifest.schema.json）。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PluginManifest {
    pub id: String,
    pub name: String,
    pub version: String,

    #[serde(default)]
    pub description: Option<String>,

    #[serde(default)]
    pub network: NetworkPolicy,

    /// CAP_* 的字符串名，见 CapSet / capability_from_name()。
    #[serde(default)]
    pub capabilities: Vec<String>,

    /// 前端布局：内联 JSON 或 "ui/layout.json"。
    #[serde(default)]
    pub ui_layout: Option<Value>,

    /// 申请的沙盒根。未签名时忽略；签名时也必须位于 /sandbox/ 下。
    #[serde(default)]
    pub sandbox_root: Option<String>,

    #[serde(default)]
    pub tools: Vec<ToolSpec>,
}

/// 加载时算出的最终策略。**不是** manifest 里写什么就是什么。
#[derive(Debug, Clone)]
pub struct PluginPolicy {
    pub signed: bool,
    pub capabilities: CapSet,
    pub sandbox_root: String,
    pub allow_hosts: AllowList,
}

impl PluginPolicy {
    pub fn from_manifest(manifest: &PluginManifest, signed: bool) -> Result<Self, RuntimeError> {
        if !valid_plugin_id(&manifest.id) {
            return Err(RuntimeError::InvalidManifest(format!(
                "invalid plugin id {:?} (expected ^[a-z][a-z0-9._-]{{1,31}}$)",
                manifest.id
            )));
        }
        if !valid_version(&manifest.version) {
            return Err(RuntimeError::InvalidManifest(format!(
                "invalid version {:?} (expected MAJOR.MINOR.PATCH)",
                manifest.version
            )));
        }

        let mut capabilities = CapSet::EMPTY;
        for cap in &manifest.capabilities {
            if cap == "ipc" || cap == "sys.ipc" {
                return Err(RuntimeError::InvalidManifest(
                    "CAP_IPC rejected: NexOS is single-process with zero IPC".to_string(),
                ));
            }
            let bit = capability_from_name(cap).ok_or_else(|| {
                RuntimeError::InvalidManifest(format!("unknown capability {cap:?}"))
            })?;
            capabilities = capabilities.union(bit);
        }

        // 铁律：未签名 = 不能写文件系统。manifest 申请了也没用。
        if !signed {
            capabilities = capabilities.strip(CapSet::FS_WRITE);
        }

        let fallback = format!("/sandbox/{}", manifest.id);
        let sandbox_root = match manifest.sandbox_root.as_deref() {
            Some(root) if signed => normalize_sandbox_root(root).unwrap_or(fallback),
            _ => fallback,
        };

        let allow_hosts = AllowList::parse(&manifest.network.allow_hosts)?;

        Ok(Self {
            signed,
            capabilities,
            sandbox_root,
            allow_hosts,
        })
    }
}

fn valid_plugin_id(id: &str) -> bool {
    let bytes = id.as_bytes();
    if bytes.len() < 2 || bytes.len() >= 32 {
        return false;
    }
    if !bytes[0].is_ascii_lowercase() {
        return false;
    }
    bytes[1..]
        .iter()
        .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || *b == b'.' || *b == b'_' || *b == b'-')
}

fn valid_tool_name(name: &str) -> bool {
    let bytes = name.as_bytes();
    if bytes.len() < 2 || bytes.len() > 64 {
        return false;
    }
    if !bytes[0].is_ascii_lowercase() {
        return false;
    }
    bytes[1..]
        .iter()
        .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || *b == b'_')
}

fn valid_version(v: &str) -> bool {
    let parts: Vec<&str> = v.split('.').collect();
    if parts.len() != 3 {
        return false;
    }
    parts
        .iter()
        .all(|p| !p.is_empty() && p.len() <= 9 && p.bytes().all(|b| b.is_ascii_digit()))
}

/// 与内核 pl_norm_path() 同语义：绝对路径、拒绝 ".."、拒绝反斜杠与控制符。
fn normalize_sandbox_root(root: &str) -> Option<String> {
    if !root.starts_with('/') || root.len() > 96 {
        return None;
    }
    let mut out = String::with_capacity(root.len());
    out.push('/');
    let mut first = true;
    for seg in root.split('/').filter(|s| !s.is_empty()) {
        if seg == "." {
            continue;
        }
        if seg == ".." || seg.contains('\\') || seg.bytes().any(|b| b < 0x20) {
            return None;
        }
        if !first {
            out.push('/');
        }
        first = false;
        out.push_str(seg);
    }
    if !out.starts_with("/sandbox/") || out == "/sandbox" {
        return None;
    }
    Some(out)
}

// =====================================================================
//  3. 网络白名单（URL 解析 + HostPattern）
// =====================================================================

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetError {
    BadUrl(&'static str),
    HostNotAllowed { host: String },
}

impl fmt::Display for NetError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            NetError::BadUrl(why) => write!(f, "invalid url: {why}"),
            NetError::HostNotAllowed { host } => {
                write!(f, "host {host:?} is not in the plugin's allow_hosts")
            }
        }
    }
}

impl std::error::Error for NetError {}

#[derive(Debug, Clone, PartialEq, Eq)]
enum HostPattern {
    /// 精确匹配（大小写不敏感）。
    Exact(String),
    /// `*.example.com`：只匹配至少一级子域，不匹配 apex。
    Subdomain(String),
}

/// 域名白名单。构造时就把非法 pattern 拒绝掉，避免运行时“宽松匹配”。
#[derive(Debug, Clone, Default)]
pub struct AllowList {
    patterns: Vec<HostPattern>,
}

impl AllowList {
    pub fn parse(hosts: &[String]) -> Result<Self, RuntimeError> {
        let mut patterns = Vec::with_capacity(hosts.len());
        for raw in hosts {
            let trimmed = raw.trim();
            if trimmed.is_empty() {
                return Err(RuntimeError::InvalidManifest(
                    "empty allow_hosts entry".to_string(),
                ));
            }
            patterns.push(parse_host_pattern(trimmed).map_err(|e| {
                RuntimeError::InvalidManifest(format!("bad allow_hosts entry {raw:?}: {e}"))
            })?);
        }
        Ok(Self { patterns })
    }

    pub fn is_empty(&self) -> bool {
        self.patterns.is_empty()
    }

    /// 校验完整 URL。只接受 http/https；拒绝 userinfo、非 ASCII 主机。
    pub fn check_url(&self, url: &str) -> Result<(), NetError> {
        let parsed = ParsedUrl::parse(url)?;
        self.check_host(&parsed.host)
    }

    pub fn check_host(&self, host: &str) -> Result<(), NetError> {
        let host = host.trim().trim_end_matches('.').to_ascii_lowercase();
        if !is_valid_host(&host) {
            return Err(NetError::HostNotAllowed { host });
        }
        // 空白名单 = 禁止所有网络（fail closed）。
        if self.patterns.is_empty() {
            return Err(NetError::HostNotAllowed { host });
        }
        for pattern in &self.patterns {
            let matched = match pattern {
                HostPattern::Exact(want) => host == *want,
                HostPattern::Subdomain(suffix) => {
                    // 关键：必须带 '.' 边界，且不能是 apex。
                    host.len() > suffix.len() + 1
                        && host.ends_with(suffix)
                        && host.as_bytes()[host.len() - suffix.len() - 1] == b'.'
                        && host.parse::<IpAddr>().is_err()
                }
            };
            if matched {
                return Ok(());
            }
        }
        Err(NetError::HostNotAllowed { host })
    }
}

fn parse_host_pattern(raw: &str) -> Result<HostPattern, String> {
    let p = raw.trim().trim_end_matches('.').to_ascii_lowercase();
    if p.is_empty() {
        return Err("empty pattern".to_string());
    }
    if p.contains('/') || p.contains(':') || p.contains('@') || p.contains('%') {
        return Err("pattern must be a bare hostname (no scheme/port/path)".to_string());
    }
    if let Some(rest) = p.strip_prefix("*.") {
        if rest.is_empty() || rest.contains('*') {
            return Err("wildcard must be exactly '*.' + domain".to_string());
        }
        if !is_valid_host(rest) || rest.parse::<IpAddr>().is_ok() {
            return Err("wildcard base is not a valid domain".to_string());
        }
        return Ok(HostPattern::Subdomain(rest.to_string()));
    }
    if p.contains('*') {
        return Err("wildcard is only allowed as a leading '*.'".to_string());
    }
    if !is_valid_host(&p) {
        return Err("not a valid hostname".to_string());
    }
    Ok(HostPattern::Exact(p))
}

fn is_valid_host(host: &str) -> bool {
    if host.is_empty() || host.len() > 253 {
        return false;
    }
    if !host.is_ascii() {
        // 拒绝 IDN，避免 homograph；需要 IDN 时先用 punycode 编码。
        return false;
    }
    if host.parse::<IpAddr>().is_ok() {
        return true;
    }
    for label in host.split('.') {
        if label.is_empty() || label.len() > 63 {
            return false;
        }
        let bytes = label.as_bytes();
        if bytes[0] == b'-' || bytes[bytes.len() - 1] == b'-' {
            return false;
        }
        if !bytes
            .iter()
            .all(|b| b.is_ascii_alphanumeric() || *b == b'-')
        {
            return false;
        }
    }
    true
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ParsedUrl {
    scheme: String,
    host: String,
    port: u16,
    path_and_query: String,
}

impl ParsedUrl {
    fn parse(url: &str) -> Result<Self, NetError> {
        let url = url.trim();
        if url.is_empty() || url.len() > 2048 {
            return Err(NetError::BadUrl("empty or too long"));
        }
        let (scheme_raw, rest) = url
            .split_once("://")
            .ok_or(NetError::BadUrl("missing scheme://"))?;
        let scheme = scheme_raw.to_ascii_lowercase();
        if scheme != "http" && scheme != "https" {
            return Err(NetError::BadUrl("only http and https are allowed"));
        }
        if rest.is_empty() {
            return Err(NetError::BadUrl("empty authority"));
        }

        // 去掉 fragment（不参与请求行）。
        let rest = rest.split('#').next().unwrap_or(rest);

        let cut = rest
            .char_indices()
            .find(|(_, c)| *c == '/' || *c == '?')
            .map(|(i, _)| i)
            .unwrap_or(rest.len());
        let authority = &rest[..cut];
        let path_and_query = if cut == rest.len() {
            "/".to_string()
        } else if rest.as_bytes()[cut] == b'?' {
            format!("/{}", &rest[cut..])
        } else {
            rest[cut..].to_string()
        };

        if authority.is_empty() || authority.contains('@') {
            return Err(NetError::BadUrl("userinfo is not allowed"));
        }
        if authority
            .bytes()
            .any(|b| b == b'%' || b == b'\\' || b <= 0x20 || b == 0x7f)
        {
            return Err(NetError::BadUrl("control/escaped characters in host"));
        }

        let (host_raw, port_raw) = if let Some(inner) = authority.strip_prefix('[') {
            let close = inner
                .find(']')
                .ok_or(NetError::BadUrl("unterminated IPv6 literal"))?;
            let host = &inner[..close];
            let tail = &inner[close + 1..];
            let port = if tail.is_empty() {
                None
            } else {
                Some(
                    tail.strip_prefix(':')
                        .ok_or(NetError::BadUrl("garbage after IPv6 literal"))?,
                )
            };
            (host, port)
        } else if let Some((h, p)) = authority.rsplit_once(':') {
            if p.is_empty() || !p.bytes().all(|b| b.is_ascii_digit()) {
                return Err(NetError::BadUrl("invalid port"));
            }
            (h, Some(p))
        } else {
            (authority, None)
        };

        let host = host_raw.trim_end_matches('.').to_ascii_lowercase();
        if host.is_empty() || !is_valid_host(&host) {
            return Err(NetError::BadUrl("invalid host"));
        }

        let default_port = if scheme == "https" { 443 } else { 80 };
        let port = match port_raw {
            None => default_port,
            Some(p) => p
                .parse::<u16>()
                .ok()
                .filter(|p| *p > 0)
                .ok_or(NetError::BadUrl("port out of range"))?,
        };

        Ok(Self {
            scheme,
            host,
            port,
            path_and_query,
        })
    }
}

/// 授权后的请求目标。插件拿到它之后才允许交给 HTTP 客户端。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthorizedRequest {
    pub scheme: String,
    pub host: String,
    pub port: u16,
    pub path_and_query: String,
}

impl AuthorizedRequest {
    pub fn to_url(&self) -> String {
        let host = if self.host.contains(':') {
            format!("[{}]", self.host)
        } else {
            self.host.clone()
        };
        let default = if self.scheme == "https" { 443 } else { 80 };
        if self.port == default {
            format!("{}://{}{}", self.scheme, host, self.path_and_query)
        } else {
            format!(
                "{}://{}:{}{}",
                self.scheme, host, self.port, self.path_and_query
            )
        }
    }
}

/// 每个插件调用都拿到一个 HttpGuard。插件**必须**先 authorize()，
/// 不允许把原始 URL 直接交给客户端。
#[derive(Debug, Clone)]
pub struct HttpGuard {
    plugin_id: Arc<str>,
    allow: Arc<AllowList>,
}

impl HttpGuard {
    pub fn new(plugin_id: Arc<str>, allow: AllowList) -> Self {
        Self {
            plugin_id,
            allow: Arc::new(allow),
        }
    }

    pub fn plugin_id(&self) -> &str {
        &self.plugin_id
    }

    pub fn allow_hosts(&self) -> &AllowList {
        &self.allow
    }

    /// 唯一合法的出站入口。
    pub fn authorize(&self, url: &str) -> Result<AuthorizedRequest, NetError> {
        let parsed = ParsedUrl::parse(url)?;
        self.allow.check_host(&parsed.host)?;
        Ok(AuthorizedRequest {
            scheme: parsed.scheme,
            host: parsed.host,
            port: parsed.port,
            path_and_query: parsed.path_and_query,
        })
    }

    pub fn check_host(&self, host: &str) -> Result<(), NetError> {
        self.allow.check_host(host)
    }
}

// =====================================================================
//  4. 插件条目 / 运行时
// =====================================================================

pub type ToolFuture = Pin<Box<dyn Future<Output = ToolResult> + Send + 'static>>;
pub type ToolHandler = Arc<dyn Fn(CallContext, Value) -> ToolFuture + Send + Sync + 'static>;

/// 传给插件的调用上下文：身份、能力、沙盒根、网络守卫。
#[derive(Clone)]
pub struct CallContext {
    pub plugin_id: Arc<str>,
    pub tool: String,
    pub capabilities: CapSet,
    pub sandbox_root: Arc<str>,
    pub net: HttpGuard,
}

impl fmt::Debug for CallContext {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CallContext")
            .field("plugin_id", &self.plugin_id)
            .field("tool", &self.tool)
            .field("capabilities", &self.capabilities)
            .field("sandbox_root", &self.sandbox_root)
            .finish()
    }
}

struct PluginEntry {
    manifest: PluginManifest,
    policy: PluginPolicy,
    tools: HashMap<String, ToolSpec>,
    handler: ToolHandler,
    inflight: Arc<AtomicU64>,
}

#[derive(Debug, Clone)]
pub struct RuntimeLimits {
    /// 单次 tools/call 的墙钟超时。
    pub call_timeout: Duration,
    /// 同时在跑的插件调用上限（超过返回 Busy，而不是排队把内存吃光）。
    pub max_concurrent_calls: usize,
}

impl Default for RuntimeLimits {
    fn default() -> Self {
        Self {
            call_timeout: Duration::from_secs(30),
            max_concurrent_calls: 32,
        }
    }
}

struct RuntimeInner {
    plugins: RwLock<HashMap<String, Arc<PluginEntry>>>,
    limits: RuntimeLimits,
    permits: Arc<Semaphore>,
}

/// 宿主侧运行时。Clone 是廉价的（内部 Arc）。
#[derive(Clone)]
pub struct PluginRuntime {
    inner: Arc<RuntimeInner>,
}

impl PluginRuntime {
    pub fn new(limits: RuntimeLimits) -> Self {
        let permits = Arc::new(Semaphore::new(limits.max_concurrent_calls.max(1)));
        Self {
            inner: Arc::new(RuntimeInner {
                plugins: RwLock::new(HashMap::new()),
                limits,
                permits,
            }),
        }
    }

    pub fn limits(&self) -> &RuntimeLimits {
        &self.inner.limits
    }

    /// 从 manifest.json 文本装载插件。`signed` 必须由**签名校验之后**的
    /// 安装器传入；插件自己无法伪造。
    pub async fn install(
        &self,
        manifest_json: &str,
        signed: bool,
        handler: ToolHandler,
    ) -> Result<String, RuntimeError> {
        let manifest: PluginManifest = serde_json::from_str(manifest_json)
            .map_err(|e| RuntimeError::InvalidManifest(format!("manifest json: {e}")))?;
        self.install_manifest(manifest, signed, handler).await
    }

    pub async fn install_manifest(
        &self,
        manifest: PluginManifest,
        signed: bool,
        handler: ToolHandler,
    ) -> Result<String, RuntimeError> {
        let policy = PluginPolicy::from_manifest(&manifest, signed)?;

        let mut tools = HashMap::new();
        for tool in &manifest.tools {
            if !valid_tool_name(&tool.name) {
                return Err(RuntimeError::InvalidManifest(format!(
                    "invalid tool name {:?} (expected ^[a-z][a-z0-9_]{{1,63}}$)",
                    tool.name
                )));
            }
            if tools.insert(tool.name.clone(), tool.clone()).is_some() {
                return Err(RuntimeError::InvalidManifest(format!(
                    "duplicate tool name {:?}",
                    tool.name
                )));
            }
        }

        let id = manifest.id.clone();
        let entry = Arc::new(PluginEntry {
            manifest,
            policy,
            tools,
            handler,
            inflight: Arc::new(AtomicU64::new(0)),
        });

        let mut plugins = self.inner.plugins.write().await;
        if plugins.contains_key(&id) {
            return Err(RuntimeError::DuplicatePlugin(id));
        }
        plugins.insert(id.clone(), entry);
        Ok(id)
    }

    pub async fn uninstall(&self, plugin_id: &str) -> Result<(), RuntimeError> {
        let mut plugins = self.inner.plugins.write().await;
        plugins
            .remove(plugin_id)
            .map(|_| ())
            .ok_or_else(|| RuntimeError::UnknownPlugin(plugin_id.to_string()))
    }

    pub async fn plugin_ids(&self) -> Vec<String> {
        let plugins = self.inner.plugins.read().await;
        let mut ids: Vec<String> = plugins.keys().cloned().collect();
        ids.sort();
        ids
    }

    pub async fn policy(&self, plugin_id: &str) -> Option<PluginPolicy> {
        let plugins = self.inner.plugins.read().await;
        plugins.get(plugin_id).map(|e| e.policy.clone())
    }

    pub async fn ui_layout(&self, plugin_id: &str) -> Option<Value> {
        let plugins = self.inner.plugins.read().await;
        plugins.get(plugin_id).and_then(|e| e.manifest.ui_layout.clone())
    }

    async fn lookup(&self, plugin_id: &str) -> Result<Arc<PluginEntry>, RuntimeError> {
        let plugins = self.inner.plugins.read().await;
        plugins
            .get(plugin_id)
            .cloned()
            .ok_or_else(|| RuntimeError::UnknownPlugin(plugin_id.to_string()))
    }

    /// 当前在飞调用数（测试/诊断用）。
    pub async fn inflight(&self, plugin_id: &str) -> u64 {
        let plugins = self.inner.plugins.read().await;
        plugins
            .get(plugin_id)
            .map(|e| e.inflight.load(Ordering::Relaxed))
            .unwrap_or(0)
    }

    /// 执行一次工具调用。
    ///
    /// 顺序：查插件 -> 查工具 -> 查能力位 -> 抢并发许可 -> 起任务
    ///       -> 超时/abort -> 解包 JoinHandle。
    pub async fn call_tool(
        &self,
        plugin_id: &str,
        tool: &str,
        args: Value,
    ) -> Result<ToolResult, RuntimeError> {
        let entry = self.lookup(plugin_id).await?;

        if !entry.tools.contains_key(tool) {
            return Err(RuntimeError::UnknownTool {
                plugin: plugin_id.to_string(),
                tool: tool.to_string(),
            });
        }

        if let Some(required) = tool_requirement(tool) {
            if !entry.policy.capabilities.contains(required) {
                return Err(RuntimeError::CapabilityDenied {
                    plugin: plugin_id.to_string(),
                    tool: tool.to_string(),
                    required: format!("{:#x}", required.bits()),
                });
            }
        }

        let permit = self
            .inner
            .permits
            .clone()
            .try_acquire_owned()
            .map_err(|_| RuntimeError::Busy)?;

        let plugin_arc: Arc<str> = Arc::from(plugin_id);
        let ctx = CallContext {
            plugin_id: plugin_arc.clone(),
            tool: tool.to_string(),
            capabilities: entry.policy.capabilities,
            sandbox_root: Arc::from(entry.policy.sandbox_root.as_str()),
            net: HttpGuard::new(plugin_arc, entry.policy.allow_hosts.clone()),
        };

        let handler = entry.handler.clone();
        let inflight = entry.inflight.clone();
        inflight.fetch_add(1, Ordering::Relaxed);

        let panic_label = format!("{plugin_id}/{tool}");

        let mut handle: JoinHandle<ToolResult> = tokio::spawn(async move {
            let _permit = permit;
            let _guard = InflightGuard(inflight);

            // catch_unwind 包住 Future 的**每一次 poll**，panic 在这里变成
            // Err，而不是让整个 worker 线程 unwind 到 tokio 的调度器。
            match CatchUnwind::new(handler(ctx, args)).await {
                Ok(result) => result,
                Err(payload) => ToolResult::error(format!(
                    "plugin {panic_label} panicked: {}",
                    panic_message(payload.as_ref())
                )),
            }
        });

        let deadline = self.inner.limits.call_timeout;
        match timeout(deadline, &mut handle).await {
            Ok(Ok(result)) => Ok(result),
            Ok(Err(join_error)) => {
                if join_error.is_panic() {
                    Err(RuntimeError::Panic {
                        plugin: plugin_id.to_string(),
                    })
                } else {
                    Err(RuntimeError::Cancelled {
                        plugin: plugin_id.to_string(),
                    })
                }
            }
            Err(_elapsed) => {
                // 先 abort，再 await：返回时任务已经被 drop，不会在调用方
                // 拿到 Timeout 之后继续写共享状态。
                handle.abort();
                let _ = handle.await;
                Err(RuntimeError::Timeout(deadline))
            }
        }
    }

    /// MCP tools/list 的结果（含非标准扩展字段 x-nexos-plugin）。
    pub async fn list_tools(&self) -> Value {
        let plugins = self.inner.plugins.read().await;
        let mut tools = Vec::new();
        let mut ids: Vec<&String> = plugins.keys().collect();
        ids.sort();
        for id in ids {
            let entry = &plugins[id];
            for tool in &entry.manifest.tools {
                tools.push(json!({
                    "name": tool.name,
                    "description": tool.description,
                    "inputSchema": if tool.input_schema.is_null() {
                        json!({"type": "object"})
                    } else {
                        tool.input_schema.clone()
                    },
                    "x-nexos-plugin": id,
                }));
            }
        }
        json!({ "tools": tools })
    }

    // -----------------------------------------------------------------
    //  JSON-RPC 2.0 / MCP 适配
    // -----------------------------------------------------------------

    /// 处理一条 MCP 报文。返回 Value::Null 表示“通知，不应有响应”。
    pub async fn dispatch_jsonrpc(&self, request: &Value) -> Value {
        let id = request.get("id").cloned();
        let id_value = id.clone().unwrap_or(Value::Null);
        let is_notification = id.is_none();

        if request.get("jsonrpc").and_then(Value::as_str) != Some("2.0") {
            return rpc_error(id_value, -32600, "invalid jsonrpc version", None);
        }
        let Some(method) = request.get("method").and_then(Value::as_str) else {
            return rpc_error(id_value, -32600, "missing method", None);
        };

        let outcome: Result<Value, RpcFailure> = match method {
            "initialize" => Ok(json!({
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": { "tools": { "listChanged": true } },
                "serverInfo": {
                    "name": "nexos-plugin-runtime",
                    "version": env!("CARGO_PKG_VERSION"),
                },
                "instructions": "NexOS single-process plugin runtime. Tools are namespaced by x-nexos-plugin."
            })),
            "notifications/initialized" => return Value::Null,
            "ping" => Ok(json!({})),
            "tools/list" => Ok(self.list_tools().await),
            "tools/call" => self.handle_tools_call(request.get("params")).await,
            _ => Err(RpcFailure::new(
                -32601,
                format!("method not found: {method}"),
            )),
        };

        // JSON-RPC 通知没有 id，不得回包（即使执行失败）。
        if is_notification {
            return Value::Null;
        }

        match outcome {
            Ok(result) => json!({ "jsonrpc": "2.0", "id": id_value, "result": result }),
            Err(failure) => failure.into_response(id_value),
        }
    }

    async fn handle_tools_call(&self, params: Option<&Value>) -> Result<Value, RpcFailure> {
        let Some(params) = params else {
            return Err(RpcFailure::new(-32602, "missing params"));
        };
        if !params.is_object() {
            return Err(RpcFailure::new(-32602, "params must be an object"));
        }
        let Some(name) = params.get("name").and_then(Value::as_str) else {
            return Err(RpcFailure::new(-32602, "missing params.name"));
        };
        let arguments = match params.get("arguments") {
            None | Some(Value::Null) => json!({}),
            Some(Value::Object(_)) => params["arguments"].clone(),
            Some(_) => return Err(RpcFailure::new(-32602, "arguments must be an object")),
        };

        let explicit_plugin = params
            .get("pluginId")
            .or_else(|| params.get("plugin_id"))
            .and_then(Value::as_str);

        let plugin_id = match explicit_plugin {
            Some(p) => p.to_string(),
            None => self.resolve_plugin_for_tool(name).await?,
        };

        let result = self
            .call_tool(&plugin_id, name, arguments)
            .await
            .map_err(RpcFailure::from_runtime)?;

        Ok(result.to_result_json())
    }

    /// 没有显式 pluginId 时按工具名反查；0 个 -> -32004，多个 -> -32602。
    async fn resolve_plugin_for_tool(&self, tool: &str) -> Result<String, RpcFailure> {
        let plugins = self.inner.plugins.read().await;
        let mut found: Vec<&String> = plugins
            .iter()
            .filter(|(_, e)| e.tools.contains_key(tool))
            .map(|(id, _)| id)
            .collect();
        match found.len() {
            0 => Err(RpcFailure::new(
                -32004,
                format!("unknown tool {tool:?} (pass params.pluginId to disambiguate)"),
            )),
            1 => Ok(found.remove(0).clone()),
            _ => Err(RpcFailure::new(
                -32602,
                format!("tool {tool:?} exists in multiple plugins; pass params.pluginId"),
            )),
        }
    }
}

struct InflightGuard(Arc<AtomicU64>);

impl Drop for InflightGuard {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::Relaxed);
    }
}

// =====================================================================
//  5. panic 隔离：把 Future 的 poll 包进 catch_unwind
// =====================================================================

/// `futures::FutureExt::catch_unwind` 的最小实现（不额外引入 futures crate）。
///
/// 安全性说明：`inner` 是结构固定的字段，本类型不实现 Unpin、不提供
/// `&mut` 访问，也不会在 poll 之间移动它，因此 `Pin::new_unchecked`
/// 满足 pin 合约。
struct CatchUnwind<F> {
    inner: F,
}

impl<F> CatchUnwind<F> {
    fn new(inner: F) -> Self {
        Self { inner }
    }
}

impl<F: Future> Future for CatchUnwind<F> {
    type Output = Result<F::Output, Box<dyn Any + Send + 'static>>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // SAFETY: see the type-level comment above.
        let this = unsafe { self.get_unchecked_mut() };
        let fut = unsafe { Pin::new_unchecked(&mut this.inner) };
        match catch_unwind(AssertUnwindSafe(|| fut.poll(cx))) {
            Ok(Poll::Ready(value)) => Poll::Ready(Ok(value)),
            Ok(Poll::Pending) => Poll::Pending,
            Err(payload) => Poll::Ready(Err(payload)),
        }
    }
}

fn panic_message(payload: &(dyn Any + Send)) -> String {
    if let Some(s) = payload.downcast_ref::<&'static str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "non-string panic payload".to_string()
    }
}

// =====================================================================
//  6. 错误 -> JSON-RPC
// =====================================================================

#[derive(Debug)]
enum RpcFailure {
    Code {
        code: i64,
        message: String,
        data: Option<Value>,
    },
}

impl RpcFailure {
    fn new(code: i64, message: impl Into<String>) -> Self {
        Self::Code {
            code,
            message: message.into(),
            data: None,
        }
    }

    fn with_data(code: i64, message: impl Into<String>, data: Value) -> Self {
        Self::Code {
            code,
            message: message.into(),
            data: Some(data),
        }
    }

    fn from_runtime(error: RuntimeError) -> Self {
        let data = json!({ "kind": error.kind() });
        let code = error.code();
        let message = error.to_string();
        match error {
            RuntimeError::UnknownTool { .. } | RuntimeError::CapabilityDenied { .. } => {
                Self::with_data(code, message, data)
            }
            _ => Self::new(code, message),
        }
    }

    fn into_response(self, id: Value) -> Value {
        match self {
            RpcFailure::Code {
                code,
                message,
                data,
            } => rpc_error(id, code, &message, data),
        }
    }
}

fn rpc_error(id: Value, code: i64, message: &str, data: Option<Value>) -> Value {
    let mut error = json!({ "code": code, "message": message });
    if let Some(data) = data {
        error["data"] = data;
    }
    json!({ "jsonrpc": "2.0", "id": id, "error": error })
}

/// 宿主侧运行时错误。`code()` 是 JSON-RPC 错误码。
#[derive(Debug)]
pub enum RuntimeError {
    InvalidManifest(String),
    DuplicatePlugin(String),
    UnknownPlugin(String),
    UnknownTool { plugin: String, tool: String },
    CapabilityDenied {
        plugin: String,
        tool: String,
        required: String,
    },
    Busy,
    Timeout(Duration),
    Panic { plugin: String },
    Cancelled { plugin: String },
    Net(NetError),
    Json(String),
}

impl RuntimeError {
    pub fn code(&self) -> i64 {
        match self {
            RuntimeError::InvalidManifest(_) | RuntimeError::Json(_) => -32602,
            RuntimeError::UnknownPlugin(_) | RuntimeError::UnknownTool { .. } => -32004,
            RuntimeError::CapabilityDenied { .. } | RuntimeError::Net(_) => -32001,
            RuntimeError::Busy => -32000,
            RuntimeError::Timeout(_) => -32003,
            RuntimeError::Panic { .. } => -32005,
            RuntimeError::Cancelled { .. } => -32800,
            RuntimeError::DuplicatePlugin(_) => -32600,
        }
    }

    pub fn kind(&self) -> &'static str {
        match self {
            RuntimeError::InvalidManifest(_) => "invalid_manifest",
            RuntimeError::DuplicatePlugin(_) => "duplicate_plugin",
            RuntimeError::UnknownPlugin(_) => "unknown_plugin",
            RuntimeError::UnknownTool { .. } => "unknown_tool",
            RuntimeError::CapabilityDenied { .. } => "capability_denied",
            RuntimeError::Busy => "busy",
            RuntimeError::Timeout(_) => "timeout",
            RuntimeError::Panic { .. } => "panic",
            RuntimeError::Cancelled { .. } => "cancelled",
            RuntimeError::Net(_) => "network_denied",
            RuntimeError::Json(_) => "json",
        }
    }

    /// 转成 JSON-RPC 错误对象（便于自定义服务器直接返回）。
    pub fn to_jsonrpc(&self, id: Value) -> Value {
        rpc_error(id, self.code(), &self.to_string(), Some(json!({ "kind": self.kind() })))
    }
}

impl fmt::Display for RuntimeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RuntimeError::InvalidManifest(m) => write!(f, "invalid manifest: {m}"),
            RuntimeError::DuplicatePlugin(id) => write!(f, "plugin {id:?} already installed"),
            RuntimeError::UnknownPlugin(id) => write!(f, "unknown plugin {id:?}"),
            RuntimeError::UnknownTool { plugin, tool } => {
                write!(f, "plugin {plugin:?} does not expose tool {tool:?}")
            }
            RuntimeError::CapabilityDenied {
                plugin,
                tool,
                required,
            } => write!(
                f,
                "plugin {plugin:?} lacks capability {required} required by {tool:?}"
            ),
            RuntimeError::Busy => write!(f, "too many concurrent plugin calls"),
            RuntimeError::Timeout(d) => write!(f, "plugin call timed out after {:?}", d),
            RuntimeError::Panic { plugin } => write!(f, "plugin {plugin:?} panicked"),
            RuntimeError::Cancelled { plugin } => write!(f, "plugin {plugin:?} was cancelled"),
            RuntimeError::Net(e) => write!(f, "network policy: {e}"),
            RuntimeError::Json(e) => write!(f, "json error: {e}"),
        }
    }
}

impl std::error::Error for RuntimeError {}

impl From<NetError> for RuntimeError {
    fn from(value: NetError) -> Self {
        RuntimeError::Net(value)
    }
}

// =====================================================================
//  7. 测试
// =====================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, Ordering};
    use tokio::time::sleep;

    fn manifest_json(id: &str, caps: &str, tools: &str, hosts: &str) -> String {
        format!(
            r#"{{
                "id": "{id}",
                "name": "{id} test",
                "version": "1.0.0",
                "network": {{ "allow_hosts": {hosts} }},
                "capabilities": {caps},
                "ui_layout": {{ "version": 1, "window": {{ "w": 320, "h": 200 }} }},
                "tools": {tools}
            }}"#
        )
    }

    const SYS_TOOL: &str = r#"[{"name":"sys_info","description":"system info","inputSchema":{"type":"object"}}]"#;
    const FS_WRITE_TOOL: &str =
        r#"[{"name":"fs_write","description":"write","inputSchema":{"type":"object"}}]"#;

    fn ok_handler() -> ToolHandler {
        Arc::new(|_ctx: CallContext, _args: Value| -> ToolFuture {
            Box::pin(async { ToolResult::json(json!({ "ok": true })) })
        })
    }

    // ---------------- 网络白名单 ----------------

    #[test]
    fn allowlist_exact_and_wildcard() {
        let list = AllowList::parse(&[
            "api.example.com".to_string(),
            "*.trusted.dev".to_string(),
            "127.0.0.1".to_string(),
        ])
        .unwrap();

        assert!(list.check_url("https://api.example.com/v1").is_ok());
        assert!(list.check_url("https://API.EXAMPLE.COM./v1").is_ok());
        assert!(list.check_url("http://api.example.com:8080/x").is_ok());
        assert!(list.check_url("https://x.trusted.dev/a").is_ok());
        assert!(list.check_url("https://a.b.trusted.dev/a").is_ok());

        // 子域边界不能被后缀拼出来。
        assert!(list.check_url("https://api.example.com.evil.test/").is_err());
        assert!(list.check_url("https://eviltrusted.dev/").is_err());
        assert!(list.check_url("https://trusted.dev/").is_err());
        assert!(list.check_url("https://x.trusted.dev.evil/").is_err());

        // userinfo / 计谋 scheme / 非 ASCII 主机。
        assert!(list.check_url("https://api.example.com@evil.test/").is_err());
        assert!(list.check_url("file:///etc/passwd").is_err());
        assert!(list.check_url("https://аpi.example.com/").is_err());

        // 显式 IP 必须精确匹配。
        assert!(list.check_url("http://127.0.0.1:80/").is_ok());
        assert!(list.check_url("http://127.0.0.2/").is_err());
    }

    #[test]
    fn empty_allowlist_denies_everything() {
        let list = AllowList::parse(&[]).unwrap();
        assert!(list.check_url("https://api.example.com/").is_err());
    }

    #[test]
    fn bad_patterns_are_rejected_at_install_time() {
        for bad in [
            "https://example.com",
            "example.com/path",
            "example.com:443",
            "*",
            "foo*bar.com",
            "*.127.0.0.1",
            "",
        ] {
            let parsed = AllowList::parse(&[bad.to_string()]);
            assert!(parsed.is_err(), "pattern {bad:?} should be rejected");
        }
    }

    // ---------------- 策略 ----------------

    #[test]
    fn unsigned_manifest_loses_fs_write_and_root() {
        let json = manifest_json("sysinfo", r#"["fs.read","fs.write"]"#, SYS_TOOL, "[]");
        let manifest: PluginManifest = serde_json::from_str(&json).unwrap();
        let policy = PluginPolicy::from_manifest(&manifest, false).unwrap();
        assert!(!policy.capabilities.contains(CapSet::FS_WRITE));
        assert!(policy.capabilities.contains(CapSet::FS_READ));
        assert_eq!(policy.sandbox_root, "/sandbox/sysinfo");
        assert!(!policy.signed);
    }

    #[test]
    fn signed_manifest_keeps_write_but_cannot_leave_sandbox() {
        let mut json = manifest_json("writer", r#"["fs.write"]"#, FS_WRITE_TOOL, "[]");
        json = json.replace(
            r#""network""#,
            r#""sandbox_root": "/sandbox/writer","network""#,
        );
        let manifest: PluginManifest = serde_json::from_str(&json).unwrap();
        let policy = PluginPolicy::from_manifest(&manifest, true).unwrap();
        assert!(policy.capabilities.contains(CapSet::FS_WRITE));
        assert_eq!(policy.sandbox_root, "/sandbox/writer");

        // 请求 "/" 或 /etc 必须回落到 /sandbox/<id>。
        let mut evil = manifest.clone();
        evil.sandbox_root = Some("/".to_string());
        assert_eq!(
            PluginPolicy::from_manifest(&evil, true).unwrap().sandbox_root,
            "/sandbox/writer"
        );
        evil.sandbox_root = Some("/etc".to_string());
        assert_eq!(
            PluginPolicy::from_manifest(&evil, true).unwrap().sandbox_root,
            "/sandbox/writer"
        );
    }

    #[test]
    fn ipc_capability_is_rejected() {
        let json = manifest_json("chan", r#"["ipc"]"#, SYS_TOOL, "[]");
        let manifest: PluginManifest = serde_json::from_str(&json).unwrap();
        let err = PluginPolicy::from_manifest(&manifest, true).unwrap_err();
        assert!(err.to_string().contains("zero IPC"));
    }

    // ---------------- 超时 / abort / panic ----------------

    #[tokio::test(flavor = "current_thread")]
    async fn timeout_aborts_and_clears_inflight() {
        let rt = PluginRuntime::new(RuntimeLimits {
            call_timeout: Duration::from_millis(50),
            max_concurrent_calls: 4,
        });
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();

        let handler: ToolHandler = Arc::new(move |_ctx, _args| {
            let flag = flag.clone();
            Box::pin(async move {
                // 一个“死循环”插件：只在 sleep 的 await 点让出。
                struct Mark(Arc<AtomicBool>);
                impl Drop for Mark {
                    fn drop(&mut self) {
                        self.0.store(true, Ordering::SeqCst);
                    }
                }
                let _mark = Mark(flag);
                loop {
                    sleep(Duration::from_secs(60)).await;
                }
            })
        });

        rt.install(
            &manifest_json("slow", r#"["sys.info"]"#, SYS_TOOL, "[]"),
            true,
            handler,
        )
        .await
        .unwrap();

        let err = rt.call_tool("slow", "sys_info", json!({})).await.unwrap_err();
        assert!(matches!(err, RuntimeError::Timeout(_)), "got {err:?}");
        assert!(cancelled.load(Ordering::SeqCst), "aborted task was dropped");
        assert_eq!(rt.inflight("slow").await, 0);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn panic_is_contained_and_reported_as_tool_error() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        let handler: ToolHandler = Arc::new(|_ctx, _args| {
            Box::pin(async {
                tokio::task::yield_now().await;
                panic!("boom from plugin");
            })
        });
        rt.install(
            &manifest_json("panicky", r#"["sys.info"]"#, SYS_TOOL, "[]"),
            true,
            handler,
        )
        .await
        .unwrap();

        let result = rt.call_tool("panicky", "sys_info", json!({})).await.unwrap();
        assert!(result.is_error);
        let text = match &result.content[0] {
            ContentBlock::Text { text } => text.clone(),
            _ => panic!("expected text"),
        };
        assert!(text.contains("panicked"), "got {text}");
        assert!(text.contains("boom from plugin"));

        // 运行时还能继续服务。
        assert_eq!(rt.plugin_ids().await, vec!["panicky".to_string()]);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn unknown_tool_and_capability_are_rejected_before_spawn() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        rt.install(
            &manifest_json("sysinfo", r#"["sys.info"]"#, SYS_TOOL, "[]"),
            false,
            ok_handler(),
        )
        .await
        .unwrap();

        let err = rt.call_tool("sysinfo", "nope", json!({})).await.unwrap_err();
        assert!(matches!(err, RuntimeError::UnknownTool { .. }));

        // 未签名插件申请 fs.write 也会被策略摘掉；这里工具存在但能力不足。
        rt.install(
            &manifest_json("writer", r#"["fs.write"]"#, FS_WRITE_TOOL, "[]"),
            false,
            ok_handler(),
        )
        .await
        .unwrap();
        let err = rt
            .call_tool("writer", "fs_write", json!({"path": "/sandbox/writer/a"}))
            .await
            .unwrap_err();
        assert!(matches!(err, RuntimeError::CapabilityDenied { .. }));
    }

    // ---------------- MCP ----------------

    #[tokio::test(flavor = "current_thread")]
    async fn mcp_tools_list_and_call() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        rt.install(
            &manifest_json("sysinfo", r#"["sys.info"]"#, SYS_TOOL, "[]"),
            true,
            ok_handler(),
        )
        .await
        .unwrap();

        let list = rt
            .dispatch_jsonrpc(&json!({
                "jsonrpc": "2.0", "id": 1, "method": "tools/list"
            }))
            .await;
        assert_eq!(list["result"]["tools"][0]["name"], "sys_info");
        assert_eq!(list["result"]["tools"][0]["x-nexos-plugin"], "sysinfo");

        let call = rt
            .dispatch_jsonrpc(&json!({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {
                    "name": "sys_info",
                    "pluginId": "sysinfo",
                    "arguments": {}
                }
            }))
            .await;
        assert_eq!(call["id"], 2);
        assert_eq!(call["result"]["isError"], false);
        assert_eq!(call["result"]["structuredContent"]["ok"], true);

        let unknown = rt
            .dispatch_jsonrpc(&json!({
                "jsonrpc": "2.0", "id": 3, "method": "tools/call",
                "params": { "name": "does_not_exist", "arguments": {} }
            }))
            .await;
        assert_eq!(unknown["error"]["code"], -32004);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn notifications_get_no_response() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        let resp = rt
            .dispatch_jsonrpc(&json!({
                "jsonrpc": "2.0", "method": "notifications/initialized"
            }))
            .await;
        assert!(resp.is_null());
    }

    #[tokio::test(flavor = "current_thread")]
    async fn initialize_reports_protocol_version() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        let resp = rt
            .dispatch_jsonrpc(&json!({
                "jsonrpc": "2.0", "id": 0, "method": "initialize",
                "params": {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": {},
                    "clientInfo": { "name": "test", "version": "0" }
                }
            }))
            .await;
        assert_eq!(resp["result"]["protocolVersion"], MCP_PROTOCOL_VERSION);
        assert_eq!(resp["result"]["serverInfo"]["name"], "nexos-plugin-runtime");
    }

    #[tokio::test(flavor = "current_thread")]
    async fn http_guard_blocks_by_default() {
        let rt = PluginRuntime::new(RuntimeLimits::default());
        rt.install(
            &manifest_json("netless", r#"["net"]"#, SYS_TOOL, "[]"),
            true,
            ok_handler(),
        )
        .await
        .unwrap();

        let guard = HttpGuard::new(
            Arc::from("netless"),
            rt.policy("netless").await.unwrap().allow_hosts,
        );
        let err = guard.authorize("https://api.example.com/").unwrap_err();
        assert!(matches!(err, NetError::HostNotAllowed { .. }));
    }
}
