fn main(){
    // Build id from generated report (or ORCA_BUILD_ID env), never hardcoded
    let build_id = std::env::var("ORCA_BUILD_ID").unwrap_or_else(|_| {
        let report_path = "../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json";
        let alt_path = "C:/Users/User/CLionProjects/OrcaSlicer/cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json";
        let p = if std::path::Path::new(report_path).exists() { report_path } else { alt_path };
        let txt = std::fs::read_to_string(p).unwrap_or_else(|e| panic!("cannot read report {}: {}", p, e));
        let v: serde_json::Value = serde_json::from_str(&txt).expect("report json parse");
        v["build"]["build_id"].as_str().expect("build_id missing").to_string()
    });
    std::env::set_var("ORCA_PLUGIN_ID", "com.orca.rust-example");
    std::env::set_var("ORCA_PLUGIN_NAME", "Rust Example");
    std::env::set_var("ORCA_PLUGIN_VERSION", "0.1.0");
    std::env::set_var("ORCA_BUILD_ID", &build_id);
    std::env::set_var("ORCA_OS", "windows");
    orca_hook_build::emit();
    // Overwrite the emitted RC with correct RC syntax without windows.h include
    // to avoid rc.exe missing include path when building from plain cargo.
    let out = std::env::var("OUT_DIR").unwrap();
    let rc_path = format!("{}/orca_plugin_metadata.rc", out);
    let json = format!(r#"{{"schema":1,"id":"com.orca.rust-example","name":"Rust Example","version":"0.1.0","runtime":"native","language":"rust","hook_abi":1,"targets":[{{"os":"windows","arch":"x86_64","build_id":"{}"}}]}}"#, build_id);
    let escaped = json.replace("\"", "\"\"");
    let rc_content = format!("1 \"ORCA_PLUGIN_METADATA\"\nBEGIN\n    \"{}\\0\"\nEND\n", escaped);
    std::fs::write(&rc_path, rc_content).expect("write rc");
    let mut res = winres::WindowsResource::new();
    res.set_resource_file(&rc_path);
    if let Err(e) = res.compile() {
        eprintln!("winres compile failed: {:?} (rc_path={})", e, rc_path);
        std::process::exit(1);
    }
}
