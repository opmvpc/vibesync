// UI de référence (mode debug) : parle au moteur Go via le canal WS /ui.
// Enveloppe des messages : {type, data} — cf. docs/research/2026-08-05-ui-protocol-draft.md
"use strict";

const $ = (id) => document.getElementById(id);
const token = window.VIBESYNC_TOKEN || "";
let ws = null;
let state = null;
let lastPhase = "";

function connectUI() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ui?token=${encodeURIComponent(token)}`);
  ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    handle(msg.type, msg.data || {});
  };
  ws.onclose = () => {
    $("connect-status").textContent = "Lien avec le moteur perdu, reconnexion…";
    setTimeout(connectUI, 1000);
  };
}

function send(type, data) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type, data: data || {} }));
  }
}

function handle(type, data) {
  switch (type) {
    case "state": render(data); break;
    case "toast": toast(data.text, data.level); break;
    case "chat": addChat(data.from, data.text); break;
    case "browse": renderBrowser(data); break;
    case "error": toast(data.text, "error"); break;
  }
}

// --- Rendu ---

function fmt(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  sec = Math.floor(sec);
  const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
  const mm = String(m).padStart(h ? 2 : 1, "0");
  return (h ? `${h}:` : "") + `${mm}:${String(s).padStart(2, "0")}`;
}

function render(s) {
  state = s;
  $("vlc-missing").classList.toggle("hidden", s.vlc.available);

  const inRoom = s.phase !== "idle";
  $("screen-connect").classList.toggle("hidden", inRoom);
  $("screen-room").classList.toggle("hidden", !inRoom);
  if (!inRoom) {
    $("connect-status").textContent = s.lastError || "";
    return;
  }
  if (s.phase !== lastPhase) {
    lastPhase = s.phase;
    if (s.phase === "connected") toast("Connecté à la salle « " + s.room + " »", "info");
  }

  $("room-name").textContent = s.room || "—";
  const badge = $("conn-badge");
  badge.textContent = s.phase === "connected" ? `connecté · ${s.latencyMs} ms` : (s.retrying ? "reconnexion…" : "connexion…");
  badge.className = "badge " + (s.phase === "connected" ? "ok" : "warn");

  const drift = $("drift");
  const d = Math.abs(s.driftSec || 0);
  drift.textContent = s.vlc.running ? (s.vlc.buffering ? "bufferise…" : `sync ±${d.toFixed(2)} s${s.correcting ? " (" + s.correcting + ")" : ""}`) : "VLC non lancé";
  drift.className = "badge sync " + (!s.vlc.running ? "" : d <= 0.15 ? "ok" : d < 2 ? "warn" : "err");

  $("file-info").innerHTML = s.vlc.fileName
    ? `<strong>${esc(s.vlc.fileName)}</strong><br>${esc(s.vlc.filePath || "")}`
    : "Aucun fichier ouvert.";

  const dur = s.vlc.durationSec || 0;
  const pos = s.vlc.positionSec || 0;
  $("bar").style.width = dur > 0 ? `${Math.min(100, (pos / dur) * 100)}%` : "0";
  $("t-cur").textContent = fmt(pos);
  $("t-tot").textContent = fmt(dur);

  $("btn-playpause").textContent = s.paused ? "Lecture" : "Pause";
  const ready = $("btn-ready");
  ready.classList.toggle("on", s.ready);
  ready.textContent = s.ready ? "Prêt ✓" : "Je suis prêt";

  const ul = $("users");
  ul.innerHTML = "";
  (s.users || []).forEach((u) => {
    const li = document.createElement("li");
    li.innerHTML =
      `<span class="who ${u.id === s.selfId ? "me" : ""}">${esc(u.name)}</span>` +
      `<span class="pill ${u.ready ? "on" : ""}">${u.ready ? "prêt" : "pas prêt"}</span>` +
      `<span class="pill">${u.latencyMs || 0} ms</span>` +
      `<span class="file">${esc(u.file ? u.file.name : "—")}</span>`;
    ul.appendChild(li);
  });
}

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function toast(text, level) {
  const el = document.createElement("div");
  el.className = "toast " + (level || "info");
  el.textContent = text;
  $("toasts").appendChild(el);
  setTimeout(() => el.remove(), 5000);
}

function addChat(from, text) {
  const box = $("chat");
  const el = document.createElement("div");
  el.className = "msg";
  el.innerHTML = `<span class="from">${esc(from)}</span>${esc(text)}`;
  box.appendChild(el);
  box.scrollTop = box.scrollHeight;
}

// --- Explorateur de fichiers ---

function renderBrowser(listing) {
  $("browser").classList.remove("hidden");
  $("browser-path").textContent = listing.path;
  const roots = $("browser-roots");
  roots.innerHTML = "";
  (listing.roots || []).forEach((r) => {
    const li = document.createElement("li");
    li.textContent = "🖴 " + r.name;
    li.onclick = () => send("browse", { path: r.path });
    roots.appendChild(li);
  });
  const list = $("browser-list");
  list.innerHTML = "";
  if (listing.parent) {
    const up = document.createElement("li");
    up.textContent = "⬆ Dossier parent";
    up.onclick = () => send("browse", { path: listing.parent });
    list.appendChild(up);
  }
  (listing.entries || []).forEach((e) => {
    const li = document.createElement("li");
    li.innerHTML = `${e.isDir ? "📁" : "🎬"} ${esc(e.name)}` +
      (e.isDir ? "" : `<span class="size">${(e.sizeBytes / 1e9).toFixed(2)} Go</span>`);
    li.onclick = () => {
      if (e.isDir) { send("browse", { path: e.path }); }
      else { send("openFile", { path: e.path }); $("browser").classList.add("hidden"); }
    };
    list.appendChild(li);
  });
}

// --- Interactions ---

window.addEventListener("DOMContentLoaded", () => {
  ["server", "name", "room"].forEach((k) => {
    const v = localStorage.getItem("vibesync." + k);
    if (v) $("f-" + k).value = v;
  });

  $("form-connect").onsubmit = (e) => {
    e.preventDefault();
    const cmd = {
      server: $("f-server").value.trim(),
      name: $("f-name").value.trim(),
      room: $("f-room").value.trim(),
      password: $("f-password").value,
    };
    localStorage.setItem("vibesync.server", cmd.server);
    localStorage.setItem("vibesync.name", cmd.name);
    localStorage.setItem("vibesync.room", cmd.room);
    $("connect-status").textContent = "Connexion…";
    send("connect", cmd);
  };

  $("btn-quit").onclick = () => send("disconnect", {});
  $("btn-pick").onclick = () => send("browse", { path: "" });
  $("browser-close").onclick = () => $("browser").classList.add("hidden");
  $("btn-ready").onclick = () => send("setReady", { ready: !(state && state.ready) });
  $("btn-playpause").onclick = () => send(state && state.paused ? "play" : "pause", {});
  $("progress").onclick = (ev) => {
    if (!state || !state.vlc.durationSec) return;
    const r = ev.currentTarget.getBoundingClientRect();
    send("seek", { positionSec: ((ev.clientX - r.left) / r.width) * state.vlc.durationSec });
  };
  $("form-chat").onsubmit = (e) => {
    e.preventDefault();
    const text = $("f-chat").value.trim();
    if (!text) return;
    send("chat", { text });
    $("f-chat").value = "";
  };

  connectUI();
});
