// DQIX Enhanced relay: lets friends play together when neither can open a
// port (apartment and mobile internet, locked routers). Both games connect
// OUT to this (a WebSocket), which every network allows; a short code ties
// them together, and this passes the bytes between them. Nothing is stored.
//
//   wss://<this>/room/<CODE>?role=host     the host (one per code)
//   wss://<this>/room/<CODE>?role=join     a friend (up to 254 per code)
//
// Every message is binary: [type, id, data...]
//   0 open    to the host: friend <id> arrived
//   1 data    the bytes of one connection (host <-> friend <id>; a friend sends id 0)
//   2 close   connection <id> ended (either way)
//   3 ok      to whoever just connected: accepted (a friend learns its id);
//             its data: [1] = this relay takes type 6 (v34)
//   4 error   then the relay closes: the text says why
//   5 keep    keeps the connection open (ignored)
//   6 multi   host only: several friends' bytes in one message:
//             [id, length low, length high, bytes]... (Cloudflare counts each
//             message the relay gets, so one for all friends lasts longer)
// The game turns each friend into an ordinary TCP connection on the host's
// own PC (runtime/relay.c), so the session itself doesn't know the difference.

const CODE = /^\/room\/([A-HJ-NP-Z2-9]{8})$/;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === "/") return new Response("DQIX Enhanced relay\n");
    const m = url.pathname.match(CODE);
    if (!m) return new Response("not found\n", { status: 404 });
    if (req.headers.get("Upgrade") !== "websocket") return new Response("WebSocket only\n", { status: 426 });
    const role = url.searchParams.get("role");
    if (role !== "host" && role !== "join") return new Response("role=host or role=join\n", { status: 400 });
    try {
      return await env.ROOMS.get(env.ROOMS.idFromName(m[1])).fetch(req);
    } catch (e) {                                   // the relay's daily allowance is used up (or Cloudflare is down)
      const [client, server] = Object.values(new WebSocketPair());
      server.accept();
      server.send(say("The relay is full for today (it resets at midnight UTC). Meanwhile, join with the host's address, " +
                      "or all use a free virtual LAN (Tailscale, ZeroTier or Radmin VPN) and the host's address there."));
      server.close(4001, "full");
      return new Response(null, { status: 101, webSocket: client });
    }
  },
};

function msg(type, id, data) {
  const out = new Uint8Array(2 + (data ? data.length : 0));
  out[0] = type; out[1] = id;
  if (data) out.set(data, 2);
  return out;
}
const say = (text) => msg(4, 0, new TextEncoder().encode(text));
function safe(f) { try { f(); } catch (e) { /* the other side is gone already */ } }

export class Room {
  constructor(ctx, env) { this.ctx = ctx; }

  host() { return this.ctx.getWebSockets("host")[0]; }

  refuse(text) {
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ["refused"]);
    server.serializeAttachment({ role: "refused" });
    server.send(say(text));
    server.close(4000, "refused");
    return new Response(null, { status: 101, webSocket: client });
  }

  async fetch(req) {
    const role = new URL(req.url).searchParams.get("role");
    if (role === "host") {
      if (this.host()) return this.refuse("Someone is already hosting with this code.");
      const [client, server] = Object.values(new WebSocketPair());
      this.ctx.acceptWebSocket(server, ["host"]);
      server.serializeAttachment({ role: "host" });
      server.send(msg(3, 0, new Uint8Array([1])));
      return new Response(null, { status: 101, webSocket: client });
    }
    const host = this.host();
    if (!host) return this.refuse("Nobody is hosting with this code (check it, and that your friend's game is still waiting).");
    const used = new Set();
    for (const w of this.ctx.getWebSockets()) { const a = w.deserializeAttachment(); if (a && a.id) used.add(a.id); }
    let id = 1;
    while (used.has(id) && id < 255) id++;
    if (id >= 255) return this.refuse("This session is full.");
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ["j" + id]);
    server.serializeAttachment({ role: "join", id });
    server.send(msg(3, id, new Uint8Array([1])));
    safe(() => host.send(msg(0, id)));
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, data) {
    if (typeof data === "string") return;
    const b = new Uint8Array(data);
    if (b.length < 2) return;
    const a = ws.deserializeAttachment() || {};
    if (a.role === "host" && b[0] === 6) {                      // several friends at once
      for (let p = 2; p + 3 <= b.length; ) {
        const id = b[p], n = b[p + 1] | (b[p + 2] << 8);
        if (p + 3 + n > b.length) break;
        const to = this.ctx.getWebSockets("j" + id)[0];
        if (to) { const out = new Uint8Array(2 + n); out[0] = 1; out[1] = 0; out.set(b.subarray(p + 3, p + 3 + n), 2); safe(() => to.send(out)); }
        else safe(() => ws.send(msg(2, id)));
        p += 3 + n;
      }
      return;
    }
    if (a.role === "host") {
      const to = this.ctx.getWebSockets("j" + b[1])[0];
      if (!to) { if (b[0] === 1) safe(() => ws.send(msg(2, b[1]))); return; }
      if (b[0] === 1) { const out = b.slice(); out[1] = 0; safe(() => to.send(out)); }
      else if (b[0] === 2) safe(() => to.close(1000, "closed by the host"));
    } else if (a.role === "join") {
      const host = this.host();
      if (!host) { safe(() => { ws.send(say("The host has left.")); ws.close(1000, "no host"); }); return; }
      if (b[0] === 1) { const out = b.slice(); out[1] = a.id; safe(() => host.send(out)); }
      else if (b[0] === 2) safe(() => ws.close(1000, "closed"));
    }
  }

  gone(ws) {
    const a = ws.deserializeAttachment() || {};
    if (a.role === "host") {
      for (const w of this.ctx.getWebSockets()) {
        if (w === ws) continue;
        safe(() => { w.send(say("The host has left.")); w.close(1000, "host left"); });
      }
    } else if (a.role === "join") {
      const host = this.host();
      if (host && host !== ws) safe(() => host.send(msg(2, a.id)));
    }
    safe(() => ws.close(1000, "bye"));
  }
  webSocketClose(ws) { this.gone(ws); }
  webSocketError(ws) { this.gone(ws); }
}
