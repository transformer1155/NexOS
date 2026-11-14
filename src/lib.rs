//! NexOS application-level plugin runtime.
//!
//! See `src/plugin_runtime.rs` for the implementation and
//! `docs/MCP_PLUGIN_PROTOCOL.md` for the wire contract.

pub mod plugin_runtime;

pub use plugin_runtime::{
    AllowList, AuthorizedRequest, CallContext, CapSet, ContentBlock, HttpGuard, NetError,
    NetworkPolicy, PluginManifest, PluginPolicy, PluginRuntime, RuntimeError, RuntimeLimits,
    ToolFuture, ToolHandler, ToolResult, ToolSpec,
};
