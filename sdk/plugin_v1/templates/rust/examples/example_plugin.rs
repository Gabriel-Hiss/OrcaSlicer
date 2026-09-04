use orca_hook::{before, after, generated::CliPrintHelp};
#[before(target = "Slic3r::CLI::print_help", priority = 1200)]
fn before_print_help() {
    let mark = "[orca-hook rust before]\n";
    let _ = mark;
}
#[after(target = "Slic3r::CLI::print_help", priority = 1200)]
fn after_print_help() {
    let mark = "[orca-hook rust after]\n";
    let _ = mark;
}
#[no_mangle]
pub extern "C" fn orca_plugin_entry_v1(host: *const orca_hook::HostApi) -> i32 {
    let regs = orca_hook::hooks::take_registrations();
    let _ = (host, regs);
    0
}
#[no_mangle]
pub extern "C" fn orca_plugin_exit_v1(host: *const orca_hook::HostApi) -> i32 {
    let _ = host;
    0
}
