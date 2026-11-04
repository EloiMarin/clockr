// Optional WebSocket endpoint override.
//
// Empty means same-origin (`/ws`), which is how debugger_demo serves both the
// page and the trace stream. A deployment that hosts this page apart from a
// live debugger can point it elsewhere here (or per visit with `?ws=...`).
// `?local=1` ignores this and uses the in-browser BroadcastChannel transport.
window.DEBUGGER_WS_URL = "";
