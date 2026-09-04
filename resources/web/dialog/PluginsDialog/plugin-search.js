const pluginSearch = { query: "", caseSensitive: false, wholeWord: false };

function PluginSearchActive() {
  return pluginSearch.query.length > 0;
}

// Matchers (FuzzyRanges/WholeWordRanges) come from ../js/fuzzy-search.js, loaded before this script.
function MatchText(text, query) {
  if (!query)
    return [];
  return pluginSearch.wholeWord
    ? WholeWordRanges(text, query, pluginSearch.caseSensitive)
    : FuzzyRanges(text, query, pluginSearch.caseSensitive);
}

function ComputePluginMatch(plugin) {
  const q = pluginSearch.query;
  if (!q) return null;
  const name = plugin.label || plugin.name || plugin.id || "";
  const nameRanges = MatchText(name, q);
  const haystack = [
    name,
    String(plugin.id || ""),
    String(plugin.description || ""),
    String(plugin.author || ""),
    String(plugin.version || ""),
    String(plugin.language || ""),
    String(plugin.runtime || ""),
    String((plugin.targets || []).map(t => t.build_id).join(" ")),
    String(plugin.artifact_path || ""),
    String(plugin.artifact_hash || ""),
    String(plugin.current_build_id || ""),
    String(plugin.status || "")
  ].join("\n");
  const anyRanges = MatchText(haystack, q);
  const matched = !!(nameRanges && nameRanges.length) || !!(anyRanges && anyRanges.length);
  if (!matched) return null;
  return {
    matched: true,
    nameRanges: (nameRanges && nameRanges.length) ? nameRanges : null,
    hasCapMatch: false
  };
}

// --- widget wiring ---
let pluginSearchInput = null;
let pluginSearchClear = null;
let pluginSearchCc = null;
let pluginSearchW = null;

function InitPluginSearch() {
  pluginSearchInput = document.getElementById("plugin_search_input");
  pluginSearchClear = document.getElementById("plugin_search_clear");
  pluginSearchCc = document.getElementById("plugin_search_cc");
  pluginSearchW = document.getElementById("plugin_search_w");
  if (!pluginSearchInput)
    return;

  // common.js cancels keydowns at document level; stop propagation so the field stays editable.
  pluginSearchInput.addEventListener("keydown", (event) => event.stopPropagation());

  pluginSearchInput.addEventListener("input", OnPluginSearchInput);
  pluginSearchClear?.addEventListener("click", ClearPluginSearch);
  pluginSearchCc?.addEventListener("click", () => TogglePluginSearchFlag(pluginSearchCc, "caseSensitive"));
  pluginSearchW?.addEventListener("click", () => TogglePluginSearchFlag(pluginSearchW, "wholeWord"));
  SyncPluginSearchClear();
}

function OnPluginSearchInput() {
  pluginSearch.query = pluginSearchInput.value;
  if (!pluginSearch.query)
    ClearSearchExpandOverride();
  SyncPluginSearchClear();
  RenderPluginsIfReady();
}

function ClearPluginSearch() {
  pluginSearch.query = "";
  if (pluginSearchInput)
    pluginSearchInput.value = "";
  ClearSearchExpandOverride();
  SyncPluginSearchClear();
  RenderPluginsIfReady();
  pluginSearchInput?.focus();
}

function TogglePluginSearchFlag(button, key) {
  pluginSearch[key] = !pluginSearch[key];
  button.classList.toggle("on", pluginSearch[key]);
  button.setAttribute("aria-pressed", String(pluginSearch[key]));
  RenderPluginsIfReady();
}

// Use visibility (not display) so toggling the clear button never reflows Cc/W.
function SyncPluginSearchClear() {
  if (pluginSearchClear)
    pluginSearchClear.style.visibility = pluginSearch.query.length ? "visible" : "hidden";
}

function ClearSearchExpandOverride() {
  if (typeof searchExpandOverride !== "undefined")
    searchExpandOverride.clear();
}

function RenderPluginsIfReady() {
  if (typeof RenderPlugins === "function")
    RenderPlugins();
}

if (typeof document !== "undefined")
  document.addEventListener("DOMContentLoaded", InitPluginSearch);
