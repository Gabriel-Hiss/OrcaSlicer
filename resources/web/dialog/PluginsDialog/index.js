const pluginsById = new Map();
const matchMap = new Map();

let expandedPluginIds = new Set();
let searchExpandOverride = new Map();
let selectedPluginId = "";
let contextPluginId = "";
let activeDetailTab = "plugin-info";

let pluginList = null;
let ctxMenu = null;

const SPLIT_STORAGE_KEY = "orca.plugins.split_ratio";
const SPLIT_DEFAULT_RATIO = 0.62;
const SPLIT_MIN_LIST_PX = 180;
const SPLIT_MIN_DETAILS_PX = 160;

let contentPane = null;
let paneSplitter = null;
let splitRatio = SPLIT_DEFAULT_RATIO;

function OnInit() {
  pluginList = document.getElementById("pluginList");
  ctxMenu = document.getElementById("ctxMenu");
  contentPane = document.querySelector(".content");
  paneSplitter = document.getElementById("paneSplitter");

  const refreshBtn = document.getElementById("refresh_btn");
  if (refreshBtn) refreshBtn.addEventListener("click", () => SendMessage("refresh_plugins"));
  const installBtn = document.getElementById("explore_btn");
  if (installBtn) installBtn.addEventListener("click", () => SendMessage("install_local_plugin"));

  document.querySelectorAll(".detail-tab").forEach(btn => {
    btn.addEventListener("click", () => ActivateDetailTab(btn.dataset.tab, true));
    btn.addEventListener("keydown", OnDetailTabKeyDown);
  });

  if (pluginList) {
    pluginList.addEventListener("click", OnPluginListClick);
    pluginList.addEventListener("change", OnPluginListChange);
    pluginList.addEventListener("contextmenu", OnPluginContextMenu);
  }
  document.addEventListener("click", (e) => {
    if (ctxMenu && !ctxMenu.contains(e.target)) HideContextMenu();
  });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") HideContextMenu(); });

  splitRatio = ReadStoredSplitRatio();
  InitPaneSplitter();
  ApplySplitRatio(splitRatio);

  RequestPlugins();
}

function InitPaneSplitter() {
  if (!paneSplitter || !contentPane) return;
  let dragging = false;
  let startY = 0;
  let startRatio = splitRatio;

  paneSplitter.addEventListener("pointerdown", (e) => {
    dragging = true;
    paneSplitter.setPointerCapture(e.pointerId);
    paneSplitter.classList.add("dragging");
    document.body.classList.add("pane-resizing");
    startY = e.clientY;
    startRatio = splitRatio;
    e.preventDefault();
  });
  paneSplitter.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const rect = contentPane.getBoundingClientRect();
    const dy = e.clientY - startY;
    const newListPx = rect.height * startRatio + dy;
    const ratio = newListPx / rect.height;
    ApplySplitRatio(ratio);
  });
  const endDrag = (e) => {
    if (!dragging) return;
    dragging = false;
    paneSplitter.classList.remove("dragging");
    document.body.classList.remove("pane-resizing");
    StoreSplitRatio(splitRatio);
    if (e && paneSplitter.hasPointerCapture && e.pointerId !== undefined) {
      try { paneSplitter.releasePointerCapture(e.pointerId); } catch (_) {}
    }
  };
  paneSplitter.addEventListener("pointerup", endDrag);
  paneSplitter.addEventListener("pointercancel", endDrag);
  paneSplitter.addEventListener("dblclick", () => {
    ApplySplitRatio(SPLIT_DEFAULT_RATIO);
    StoreSplitRatio(SPLIT_DEFAULT_RATIO);
  });
  window.addEventListener("resize", () => ApplySplitRatio(splitRatio));
}

function ApplySplitRatio(ratio) {
  if (!contentPane) return;
  const h = contentPane.getBoundingClientRect().height;
  if (h <= 0) return;
  let r = Math.max(0, Math.min(1, ratio));
  const minListRatio = SPLIT_MIN_LIST_PX / h;
  const minDetailsRatio = SPLIT_MIN_DETAILS_PX / h;
  if (r < minListRatio) r = minListRatio;
  if (r > 1 - minDetailsRatio) r = 1 - minDetailsRatio;
  if (h < SPLIT_MIN_LIST_PX + SPLIT_MIN_DETAILS_PX) {
    r = 0.5;
  }
  splitRatio = r;
  contentPane.style.setProperty("--plugin-list-height", (h * r) + "px");
  SyncPluginListHeaderGutter();
}

function ReadStoredSplitRatio() {
  try {
    const v = localStorage.getItem(SPLIT_STORAGE_KEY);
    const f = parseFloat(v);
    if (isFinite(f) && f > 0 && f < 1) return f;
  } catch (_) {}
  return SPLIT_DEFAULT_RATIO;
}
function StoreSplitRatio(ratio) {
  try { localStorage.setItem(SPLIT_STORAGE_KEY, String(ratio)); } catch (_) {}
}

function ActivateDetailTab(tabId, focusTab = false) {
  activeDetailTab = tabId;
  document.querySelectorAll(".detail-tab").forEach(btn => {
    const active = btn.dataset.tab === tabId;
    btn.classList.toggle("active", active);
    btn.setAttribute("aria-selected", active ? "true" : "false");
    btn.tabIndex = active ? 0 : -1;
    if (active && focusTab) btn.focus();
  });
  document.querySelectorAll(".detail-tab-panel").forEach(panel => {
    panel.hidden = panel.dataset.panel !== tabId;
  });
}

function OnDetailTabKeyDown(event) {
  const tabs = Array.from(document.querySelectorAll(".detail-tab"));
  const idx = tabs.indexOf(event.currentTarget);
  if (event.key === "ArrowRight") {
    event.preventDefault();
    const next = tabs[(idx + 1) % tabs.length];
    ActivateDetailTab(next.dataset.tab, true);
  } else if (event.key === "ArrowLeft") {
    event.preventDefault();
    const prev = tabs[(idx - 1 + tabs.length) % tabs.length];
    ActivateDetailTab(prev.dataset.tab, true);
  }
}

function RequestPlugins() {
  SendMessage("request_plugins");
}

function SendMessage(command, payload = {}) {
  const msg = Object.assign({ command }, payload);
  if (window.wx && typeof window.wx.postMessage === "function") {
    window.wx.postMessage(JSON.stringify(msg));
  } else if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(JSON.stringify(msg));
  } else {
    console.log("SendMessage", msg);
  }
}

function HandleStudio(value) {
  let payload = null;
  if (typeof value === "string") {
    try { payload = JSON.parse(value); } catch (_) { return; }
  } else if (value && typeof value === "object") {
    payload = value;
  }
  if (!payload) return;
  const cmd = payload.command;
  if (cmd === "list_plugins") {
    const plugins = Array.isArray(payload.data) ? payload.data : [];
    window.__currentBuildId = payload.current_build_id || "";
    if (payload.sort_key !== undefined || payload.sort_order !== undefined) {
      if (typeof NormalizePluginSort === "function") {
        const ns = NormalizePluginSort(payload.sort_key, payload.sort_order);
        if (typeof pluginSort !== "undefined") pluginSort = ns;
        if (typeof RenderSortHeaders === "function") RenderSortHeaders();
      }
    }
    ApplyPlugins(plugins);
  } else if (cmd === "status_message") {
    ShowStatusMessage(payload.message || "", payload.level || "info");
  }
}

function ShowStatusMessage(message, level) {
  const bar = document.getElementById("statusBar");
  const txt = document.getElementById("statusText");
  if (!bar || !txt) return;
  const msg = String(message || "").trim();
  txt.textContent = msg;
  bar.className = "status-bar level-" + (level || "info");
  if (!msg) bar.classList.add("is-empty");
  else bar.classList.remove("is-empty");
  if (msg) {
    clearTimeout(bar._hideTimer);
    bar._hideTimer = setTimeout(() => {
      txt.textContent = "";
      bar.classList.add("is-empty");
    }, 4000);
  }
}

function ApplyPlugins(plugins) {
  pluginsById.clear();
  for (const p of plugins) {
    const id = String(p.id || "");
    if (!id) continue;
    pluginsById.set(id, p);
  }
  if (selectedPluginId && !pluginsById.has(selectedPluginId)) selectedPluginId = "";
  if (!selectedPluginId && plugins.length > 0) {
    const first = String(plugins[0].id || "");
    selectedPluginId = first;
  }
  RenderPlugins();
  RenderDetails();
}

function SyncPluginListHeaderGutter() {}

function ApplyHighlight(container, text, ranges) {
  container.textContent = "";
  if (!ranges || ranges.length === 0) {
    container.textContent = text;
    return;
  }
  let last = 0;
  for (const r of ranges) {
    const s = Math.max(0, Math.min(text.length, r[0] | 0));
    const e = Math.max(s, Math.min(text.length, (r[0] | 0) + (r[1] | 0)));
    if (s > last) container.appendChild(document.createTextNode(text.slice(last, s)));
    const mark = document.createElement("mark");
    mark.className = "plugin-search-hit";
    mark.textContent = text.slice(s, e);
    container.appendChild(mark);
    last = e;
  }
  if (last < text.length) container.appendChild(document.createTextNode(text.slice(last)));
}

function RenderPlugins() {
  if (!pluginList) return;
  pluginList.textContent = "";
  if (pluginsById.size === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No plugins installed. Click Install plugin to add a .dll/.so/.jar.";
    pluginList.appendChild(empty);
    return;
  }
  let list = Array.from(pluginsById.values());
  let filtered = list;
  if (typeof PluginSearchActive === "function" && PluginSearchActive()) {
    filtered = [];
    for (const p of list) {
      const m = ComputePluginMatch(p);
      if (m) {
        filtered.push(p);
        matchMap.set(String(p.id), m);
      }
    }
    if (filtered.length === 0) {
      const empty = document.createElement("div");
      empty.className = "empty-state";
      empty.textContent = "No plugins match the search.";
      pluginList.appendChild(empty);
      return;
    }
  }

  for (const plugin of filtered) {
    const pid = String(plugin.id);
    const match = matchMap.get(pid) || null;
    const block = document.createElement("div");
    block.className = "plugin-block" + (pid === selectedPluginId ? " selected" : "");
    block.dataset.pluginId = pid;

    const row = document.createElement("div");
    row.className = "row plugin-cols" + (pid === selectedPluginId ? " selected" : "");
    row.dataset.pluginId = pid;

    row.appendChild(CheckCell(row, plugin));
    const nameRanges = match ? match.nameRanges : null;
    row.appendChild(LabelCell(plugin, false, 0, nameRanges));
    row.appendChild(TableTextCell(plugin.version || "-"));
    row.appendChild(TableTextCell(plugin.language || "-"));
    row.appendChild(TableTextCell(plugin.runtime || "-"));
    row.appendChild(StatusCell(plugin));

    block.appendChild(row);
    pluginList.appendChild(block);
  }
}

function GetErrorText(plugin) { return String(plugin?.error || "").trim(); }
function GetStatus(plugin) { return String(plugin?.status || ""); }
function IsPluginChecked(plugin) {
  if (typeof plugin.loaded === "boolean") return plugin.loaded === true && plugin.enabled === true;
  const s = GetStatus(plugin);
  return s === "Activated" && plugin.enabled === true;
}
function IsPluginLoading(plugin) { return GetStatus(plugin) === "Loading"; }
function CheckCell(row, plugin) {
  const cell = document.createElement("div");
  cell.className = "check-cell";
  const label = document.createElement("label");
  label.className = "plugin-checkbox" + (IsPluginLoading(plugin) ? " loading" : "") + (plugin.can_toggle === false ? " disabled" : "");
  const input = document.createElement("input");
  input.type = "checkbox";
  input.className = "plugin-checkbox-input";
  input.dataset.pluginId = String(plugin.id);
  input.checked = IsPluginChecked(plugin);
  input.disabled = plugin.can_toggle === false || IsPluginLoading(plugin);
  if (plugin.status === "Incompatible" || plugin.restart_required) input.disabled = true;
  input.setAttribute("aria-label", "Enable plugin");
  const mark = document.createElement("span");
  mark.className = "plugin-checkbox-mark";
  label.appendChild(input);
  label.appendChild(mark);
  cell.appendChild(label);
  return cell;
}

function TableTextCell(text) {
  const c = document.createElement("div");
  c.textContent = text;
  return c;
}

function LabelCell(plugin, isExpanded = false, count = 0, nameRanges = null) {
  const cell = document.createElement("div");
  cell.className = "label-cell";
  const wrap = document.createElement("div");
  wrap.className = "plugin-name-wrap";
  const nameEl = document.createElement("span");
  nameEl.className = "plugin-name-text";
  const name = String(plugin.name || plugin.id || "-");
  if (nameRanges && nameRanges.length) ApplyHighlight(nameEl, name, nameRanges);
  else nameEl.textContent = name;
  wrap.appendChild(nameEl);
  const idEl = document.createElement("span");
  idEl.className = "plugin-id-text";
  idEl.textContent = String(plugin.id || "");
  idEl.style.cssText = "font-size:11px;color:var(--muted);margin-left:6px;";
  wrap.appendChild(idEl);
  cell.appendChild(wrap);
  return cell;
}

function StatusCell(plugin) {
  const cell = document.createElement("div");
  cell.className = "status-cell status-" + String(plugin.status || "inactive").toLowerCase();
  const label = document.createElement("span");
  label.className = "status-label";
  label.textContent = String(plugin.status || "Inactive");
  if (plugin.restart_required) label.textContent = "RestartRequired";
  cell.appendChild(label);
  if (GetErrorText(plugin)) {
    cell.title = GetErrorText(plugin);
  }
  return cell;
}

function RenderDetails() {
  const pid = selectedPluginId;
  const plugin = pid ? pluginsById.get(pid) : null;
  if (!plugin) {
    SetText("detailId", "-");
    SetText("detailVersion", "-");
    SetText("detailAuthor", "-");
    SetText("detailLanguage", "-");
    SetText("detailRuntime", "-");
    SetText("detailTargets", "-");
    SetText("detailCurrentBuild", window.__currentBuildId || "-");
    SetText("detailCompatible", "-");
    SetText("detailArtifactPath", "-");
    SetText("detailArtifactHash", "-");
    SetText("detailStatus", "-");
    SetText("detailDescription", "No description available");
    SetText("detailStatusBody", "Select a plugin to view details.");
    const errRow = document.getElementById("detailErrorRow");
    if (errRow) errRow.hidden = true;
    return;
  }
  SetText("detailId", String(plugin.id || "-"));
  SetText("detailVersion", String(plugin.version || "-"));
  SetText("detailAuthor", String(plugin.author || "-"));
  SetText("detailLanguage", String(plugin.language || "-"));
  SetText("detailRuntime", String(plugin.runtime || "-"));
  const targets = Array.isArray(plugin.targets) ? plugin.targets : [];
  if (targets.length === 0) SetText("detailTargets", "-");
  else {
    const t = targets.map(x => (x.os||"?")+"/"+(x.arch||"?")+":"+String(x.build_id||"").slice(0,24)).join(", ");
    SetText("detailTargets", t);
  }
  SetText("detailCurrentBuild", String(plugin.current_build_id || window.__currentBuildId || "-"));
  SetText("detailCompatible", plugin.compatible ? "Compatible" : "Incompatible");
  SetText("detailArtifactPath", String(plugin.artifact_path || "-"));
  SetText("detailArtifactHash", String(plugin.artifact_hash || "-"));
  const statusText = String(plugin.status || "-") + (plugin.restart_required ? " (restart required)" : "") + (typeof plugin.loaded === "boolean" ? (plugin.loaded ? " • loaded" : " • not loaded") : "");
  SetText("detailStatus", statusText);
  const errRow = document.getElementById("detailErrorRow");
  if (errText) {
    SetText("detailError", errText);
    if (errRow) errRow.hidden = false;
  } else {
    if (errRow) errRow.hidden = true;
  }
  const desc = String(plugin.description || "").trim();
  SetText("detailDescription", desc || "No description available");
  const diag = document.getElementById("detailStatusBody");
  if (diag) {
    if (errText) {
      diag.textContent = errText;
      diag.className = "detail-status-body detail-error-text";
    } else if (plugin.status === "Incompatible") {
      diag.textContent = "This plugin is incompatible with the current build and will not be loaded.";
      diag.className = "detail-status-body detail-warning-text";
    } else if (plugin.restart_required) {
      diag.textContent = "Restart OrcaSlicer to complete the pending unload.";
      diag.className = "detail-status-body detail-warning-text";
    } else {
      diag.textContent = "No issues detected.";
      diag.className = "detail-status-body";
    }
  }
}

function SetText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

function OnPluginListClick(event) {
  const row = event.target.closest(".row");
  if (!row) return;
  const pid = row.dataset.pluginId;
  if (!pid) return;
  if (event.target.closest(".plugin-checkbox")) return;
  selectedPluginId = pid;
  RenderPlugins();
  RenderDetails();
}

function OnPluginListChange(event) {
  const input = event.target.closest(".plugin-checkbox-input");
  if (!input) return;
  const pid = input.dataset.pluginId;
  const enabled = input.checked;
  const plugin = pluginsById.get(pid);
  if (plugin && plugin.compatible === false && enabled) {
    input.checked = false;
    ShowStatusMessage("Incompatible plugin cannot be enabled.", "error");
    return;
  }
  if (plugin && plugin.restart_required) {
    input.checked = !enabled;
    ShowStatusMessage("Restart required before toggling.", "warn");
    return;
  }
  SendMessage("toggle_plugin", { id: pid, enabled });
}

function OnPluginContextMenu(event) {
  const row = event.target.closest(".row");
  if (!row) return;
  event.preventDefault();
  const pid = row.dataset.pluginId;
  if (!pid) return;
  selectedPluginId = pid;
  RenderPlugins();
  RenderDetails();
  contextPluginId = pid;
  ShowContextMenu(event.clientX, event.clientY);
}

function ShowContextMenu(x, y) {
  if (!ctxMenu) return;
  ctxMenu.textContent = "";
  const plugin = pluginsById.get(contextPluginId);
  if (!plugin) return;
  const actions = Array.isArray(plugin.context_actions) ? plugin.context_actions : [
    {id:"reload_plugin", label:"Reload", enabled: !!plugin.compatible},
    {id:"open_folder", label:"Show in folder", enabled:true},
    {id:"delete_plugin", label:"Delete", enabled:true, danger:true}
  ];
  for (const a of actions) {
    const btn = document.createElement("button");
    btn.className = "ctx-item" + (a.danger ? " danger" : "");
    btn.textContent = a.label;
    btn.disabled = !a.enabled;
    btn.addEventListener("click", () => {
      HideContextMenu();
      SendMessage("plugin_menu_action", { id: contextPluginId, action: a.id });
    });
    ctxMenu.appendChild(btn);
  }
  ctxMenu.hidden = false;
  ctxMenu.style.left = x + "px";
  ctxMenu.style.top = y + "px";
  const rect = ctxMenu.getBoundingClientRect();
  if (rect.right > window.innerWidth) ctxMenu.style.left = (window.innerWidth - rect.width - 8) + "px";
  if (rect.bottom > window.innerHeight) ctxMenu.style.top = (window.innerHeight - rect.height - 8) + "px";
}
function HideContextMenu() { if (ctxMenu) ctxMenu.hidden = true; }
