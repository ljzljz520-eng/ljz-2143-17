// 输入策略配置台前端（零依赖 vanilla JS）
const $ = (id) => document.getElementById(id);

const COMMANDS_META = [
  { id: "open_settings", label: "打开设置（Esc 语义）", chord: "Esc" },
  { id: "toggle_fullscreen", label: "F11 全屏切换", chord: "F11" },
  { id: "restore_background", label: "恢复背景图", chord: "Ctrl+R" },
  { id: "move_left", label: "左移（有界连发）", chord: "ArrowLeft" },
  { id: "move_right", label: "右移（有界连发）", chord: "ArrowRight" },
  { id: "move_up", label: "上移（有界连发）", chord: "ArrowUp" },
  { id: "move_down", label: "下移（有界连发）", chord: "ArrowDown" },
  { id: "confirm_business", label: "现场业务确认（重放会被拦）", chord: "Enter" },
];

$("btn-reset-default").addEventListener("click", loadDefault);
$("btn-validate").addEventListener("click", () => publish(false));
$("btn-publish").addEventListener("click", () => publish(true));
$("btn-refresh").addEventListener("click", refreshAll);
$("btn-add-event").addEventListener("click", addEventRow);
$("btn-clear-events").addEventListener("click", () => { events = []; renderEvents(); });
$("btn-run-replay").addEventListener("click", runReplay);
$("btn-upload-journal").addEventListener("click", uploadJournal);
document.querySelectorAll(".tabs button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll(".tabs button").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    $("tab-" + b.dataset.tab).classList.add("active");
    if (b.dataset.tab === "gens") loadGensAndReceipts();
    if (b.dataset.tab === "confirms") loadConfirms();
  }));

function deviceId() { return $("device-id").value.trim() || "dev-001"; }
async function api(method, path, body, isRaw = false) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    if (isRaw) { opts.headers["Content-Type"] = "application/octet-stream"; opts.body = body; }
    else { opts.headers["Content-Type"] = "application/json"; opts.body = JSON.stringify(body); }
  }
  const r = await fetch(path, opts);
  let data;
  try { data = await r.json(); } catch { data = {}; }
  return { status: r.status, data };
}

async function loadDefault() {
  const { data } = await api("GET", "/api/default-payload?generation=1");
  fillForm(data);
}

function currentPayload() {
  return {
    generation: 1, // 发布时代次由服务端数据库分配
    move_initial_delay_ms: +$("p-delay").value,
    move_repeat_ms: +$("p-repeat").value,
    move_max_hold_ms: +$("p-hold").value,
    bounds_w: +$("p-w").value,
    bounds_h: +$("p-h").value,
    bindings: COMMANDS_META.map((m) => ({
      chord: $(`chord-${m.id}`).value.trim(),
      command: m.id,
    })),
  };
}

function fillForm(p) {
  $("p-delay").value = p.move_initial_delay_ms;
  $("p-repeat").value = p.move_repeat_ms;
  $("p-hold").value = p.move_max_hold_ms;
  $("p-w").value = p.bounds_w;
  $("p-h").value = p.bounds_h;
  COMMANDS_META.forEach((m) => {
    const found = p.bindings.find((b) => b.command === m.id);
    $(`chord-${m.id}`).value = found ? found.chord : m.chord;
  });
}

function renderBindingRows() {
  const tb = $("bindings-table").querySelector("tbody");
  tb.innerHTML = "";
  COMMANDS_META.forEach((m) => {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${m.label}</td>
      <td><input id="chord-${m.id}" value="${m.chord}" size="22"
         placeholder="如 F11 / Ctrl+R / ArrowLeft"></td>
      <td class="muted">物理位置名</td>`;
    tb.appendChild(tr);
  });
}

async function publish(doPublish) {
  const payload = currentPayload();
  const out = $("validate-result");
  const { status, data } = await api("POST", "/api/validate", payload);
  renderValidation(data, out);
  if (!data.ok || !doPublish) return data;
  const pub = await api("POST",
    `/api/devices/${encodeURIComponent(deviceId())}/publish`, payload);
  if (pub.status === 201) {
    out.innerHTML = `<span class="ok">已发布代次 ${pub.data.generation}
      digest=${pub.data.digest.slice(0, 16)}…</span>`;
    refreshAll();
  } else {
    renderValidation(pub.data, out);
  }
  return pub.data;
}

function renderValidation(r, out) {
  if (r.ok) {
    out.innerHTML = `<span class="ok">✓ 校验通过：无保留键/冲突/缺失问题。</span>`;
    return;
  }
  const lines = r.issues.map((i) => {
    const cls = i.code === "RESERVED" || i.code.includes("DUP") ? "bad" : "warn2";
    return `<div class="${cls}">[${i.code}] ${escapeHtml(i.detail)}</div>`;
  });
  if (r.reserved?.length)
    lines.push(`<div class="bad">保留键：${r.reserved.map(escapeHtml).join(", ")}</div>`);
  if (r.conflicts?.length)
    lines.push(...r.conflicts.map((c) =>
      `<div class="bad">冲突：${escapeHtml(c.a||c.chord||"")}(${escapeHtml(c.cmd_a||"")})
       ↔ ${escapeHtml(c.b||c.chord||"")}(${escapeHtml(c.cmd_b||"")})</div>`));
  out.innerHTML = lines.join("");
}

async function refreshAll() {
  loadDefault();
  const { data } = await api("GET", `/api/devices/${encodeURIComponent(deviceId())}`);
  $("device-status").textContent = data.envelope
    ? `当前代次 ${data.envelope.generation}` : "尚未发布";
}

async function loadGensAndReceipts() {
  const d = encodeURIComponent(deviceId());
  const [gens, recs] = await Promise.all([
    api("GET", `/api/devices/${d}/generations`),
    api("GET", `/api/devices/${d}/receipts`)]);
  $("gens-table").querySelector("tbody").innerHTML =
    (gens.data || []).map((g) => `<tr>
      <td>${g.generation}</td><td title="${g.digest}">${g.digest.slice(0, 16)}…</td>
      <td>${g.issued_at}</td><td>${g.published_by}</td>
      <td>${g.superseded ? "已被取代" : "<b>当前</b>"}</td></tr>`).join("");
  $("receipts-table").querySelector("tbody").innerHTML =
    (recs.data || []).map((r) => `<tr>
      <td>${r.generation}</td>
      <td><span class="tag ${r.status}">${r.status}</span></td>
      <td>${r.released_keys}</td><td>${escapeHtml(r.reason)}</td>
      <td>${r.received_at}</td></tr>`).join("");
}

// ---------- 实验重放 ----------
let events = [];
function addEventRow() {
  const type = $("ev-type").value;
  const t = +$("ev-t").value || 0;
  if (type === "key") {
    const mods = $("ev-mods").value.split("+").map((s) => s.trim())
      .filter(Boolean);
    events.push({ t_ms: t, type: "key", key: $("ev-key").value.trim(),
      down: $("ev-down").checked, repeat: $("ev-repeat").checked,
      composition: $("ev-composition").checked, mods });
  } else if (type === "composition") {
    events.push({ t_ms: t, type: "composition", text: $("ev-key").value });
  } else {
    events.push({ t_ms: t, type });
  }
  events.sort((a, b) => a.t_ms - b.t_ms);
  renderEvents();
}
function renderEvents() {
  $("events-table").querySelector("tbody").innerHTML = events.map((e, i) =>
    `<tr><td>${e.t_ms}</td><td>${e.type}</td>
     <td>${escapeHtml(e.key || e.text || "")}</td>
     <td>${e.down === undefined ? "" : e.down ? "↓" : "↑"}</td>
     <td>${e.repeat ? "是" : ""}</td><td>${e.composition ? "是" : ""}</td>
     <td>${(e.mods || []).join("+")}</td>
     <td><button data-i="${i}" class="del-ev">×</button></td></tr>`).join("");
  document.querySelectorAll(".del-ev").forEach((b) =>
    b.addEventListener("click", () => {
      events.splice(+b.dataset.i, 1); renderEvents();
    }));
}

async function runReplay() {
  const body = { device_id: deviceId(), events,
    generation: $("replay-gen").value ? +$("replay-gen").value : undefined };
  const { data } = await api("POST", "/api/replay/events", body);
  renderReplay(data);
}

async function uploadJournal() {
  const f = $("journal-file").files[0];
  if (!f) return alert("先选择 .islog 文件");
  const gen = $("replay-gen").value ? +$("replay-gen").value : 1;
  const buf = await f.arrayBuffer();
  const r = await fetch(`/api/replay/journal?meta=${encodeURIComponent(
      JSON.stringify({ generation: gen }))}`,
    { method: "POST", headers: { "Content-Type": "application/octet-stream" },
      body: buf });
  const data = await r.json();
  renderReplay(data, !r.ok);
}

function renderReplay(data, isError = false) {
  const out = $("replay-result");
  if (isError || data.error) {
    out.innerHTML = `<span class="bad">重放中止：${escapeHtml(data.error || "")}
      ${data.corrupt_at_frame !== undefined ?
        `（损坏帧 #${data.corrupt_at_frame}）` : ""}</span>`;
    return;
  }
  const rows = data.commands.map((c) =>
    `<tr class="${c.blocked ? "blocked" : ""}">
      <td>${c.t_ms}</td><td>${escapeHtml(c.name)}</td>
      <td>${c.origin}</td><td>${c.blocked ?
        `<span class="tag blocked">已拦截</span> ${escapeHtml(c.note||"")}` :
        "执行"}</td></tr>`).join("");
  out.innerHTML = `
    <div class="${data.blocked.length ? "warn2" : "ok"}">
      会话 #${data.session_id}｜输入帧 ${data.input_frame_count ?? events.length}
      ｜拦截伪确认 ${data.blocked.length} 次｜视口 (${data.viewport.x},${data.viewport.y})
      ｜全屏=${data.fullscreen ? "是" : "否"}｜面板=${data.panel_open ? "开" : "关"}
    </div>
    <table><thead><tr><th>t(ms)</th><th>命令</th><th>来源</th><th>处置</th></tr></thead>
    <tbody>${rows}</tbody></table>`;
}

async function loadConfirms() {
  const d = encodeURIComponent(deviceId());
  const { data } = await api("GET", `/api/devices/${d}/confirmations`);
  $("confirms-table").querySelector("tbody").innerHTML =
    (data || []).map((c) => `<tr><td>${c.id}</td><td>${c.generation}</td>
      <td>${c.confirmed_at}</td><td><b>${c.origin}</b></td>
      <td>${escapeHtml(c.detail)}</td></tr>`).join("");
}

function escapeHtml(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

renderBindingRows();
refreshAll();
renderEvents();
