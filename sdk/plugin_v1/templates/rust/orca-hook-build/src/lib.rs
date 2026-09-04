//! orca-hook-build — build.rs helper for Rust plugins.
//! Generates PE resource (Windows) or ELF note (Linux) with metadata JSON
//! and ensures only the two C ABI exports are visible.
//! The plugin crate is `cdylib`, self-contained (static deps only), no SDK DLL.

use std::{env, fs, path::Path};

/// Metadata schema v1 — must match loader's PluginDescriptor expectations.
/// Fields: schema, id, name, version, runtime, language, hook_abi, targets, entry_class (JVM only)
#[derive(serde::Serialize)]
struct PluginMeta<'a> {
    schema: u32,
    id: &'a str,
    name: &'a str,
    version: &'a str,
    runtime: &'a str,  // "native"
    language: &'a str, // "rust"
    hook_abi: u32,
    targets: Vec<Target<'a>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    description: Option<&'a str>,
}
#[derive(serde::Serialize)]
struct Target<'a> {
    os: &'a str,
    arch: &'a str,
    build_id: &'a str,
}

/// Emit metadata and export config for `build.rs`.
/// Reads env `ORCA_PLUGIN_ID`, `ORCA_PLUGIN_NAME`, `ORCA_PLUGIN_VERSION`,
/// `ORCA_BUILD_ID`, `ORCA_OS` (windows|linux) set by the per-build SDK's `config.toml`.
/// Writes to `OUT_DIR` and prints cargo:rerun-if-changed / cargo:rustc-link-arg
pub fn emit() {
    let out = env::var("OUT_DIR").unwrap();
    let id = env::var("ORCA_PLUGIN_ID").unwrap_or_else(|_| "com.example.rust-plugin".into());
    let name = env::var("ORCA_PLUGIN_NAME").unwrap_or_else(|_| "Rust Hook Example".into());
    let version = env::var("ORCA_PLUGIN_VERSION").unwrap_or_else(|_| "0.1.0".into());
    let build_id = env::var("ORCA_BUILD_ID").unwrap_or_else(|_| "windows-x86_64-unknown".into());
    let os = env::var("ORCA_OS").unwrap_or_else(|_| if cfg!(windows) { "windows" } else { "linux" }.into());

    let meta = PluginMeta {
        schema: 1,
        id: &id,
        name: &name,
        version: &version,
        runtime: "native",
        language: "rust",
        hook_abi: 1,
        targets: vec![Target { os: &os, arch: "x86_64", build_id: &build_id }],
        description: None,
    };
    let json = serde_json::to_string(&meta).unwrap();

    // Write JSON for PE resource / ELF note embedding
    let meta_path = Path::new(&out).join("orca_plugin_meta.json");
    fs::write(&meta_path, &json).unwrap();

    // Windows: emit .rc with string type "ORCA_PLUGIN_METADATA" and "" escaping per MS RC rules
    if os == "windows" {
        let escaped = json.replace('"', "\"\"");
        let rc = format!(
            "#include <windows.h>\n1 \"ORCA_PLUGIN_METADATA\"\nBEGIN\n    \"{}\\0\"\nEND\n",
            escaped
        );
        let rc_path = Path::new(&out).join("orca_plugin_metadata.rc");
        fs::write(&rc_path, rc).unwrap();
        println!("cargo:rerun-if-env-changed=ORCA_BUILD_ID");
    } else {
        // Linux: emit .S note
        let s = format!(
            ".section \".note.orca.plugin\",\"a\",@note\n.balign 4\n.long 4\n.long {len}\n.long 0x4F524341\n.asciz \"ORCA\"\n.balign 4\n.asciz \"{json}\"\n.balign 4\n",
            len = json.len() + 1,
            json = json
        );
        let s_path = Path::new(&out).join("orca_plugin_note.S");
        fs::write(&s_path, s).unwrap();
    }

    // Ensure only the two ABI exports are global (Linux version script)
    let lds = "{\n  global:\n    orca_plugin_entry_v1;\n    orca_plugin_exit_v1;\n  local:\n    *;\n};\n";
    let lds_path = Path::new(&out).join("orca_exports.lds");
    fs::write(&lds_path, lds).unwrap();

    println!("cargo:rerun-if-env-changed=ORCA_PLUGIN_ID");
    println!("cargo:rerun-if-env-changed=ORCA_BUILD_ID");
    println!("cargo:rerun-if-env-changed=ORCA_OS");

    // The actual entry points are defined in the plugin crate (cdylib) via
    // `#[no_mangle] extern \"C\" fn orca_plugin_entry_v1(...)` wrappers that
    // delegate to orca_hook::take_registrations() transactionally, with
    // catch_unwind per hook. This build helper does not emit the entry itself,
    // only the metadata that the entry will expose via resource/note.
}
