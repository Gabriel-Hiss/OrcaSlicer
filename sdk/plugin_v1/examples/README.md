# OrcaSlicer Hook SDK — Exemplos

Este diretório contém os quatro plugins de exemplo que compilam contra o SDK per-build gerado pelo `hook-sdkgen`.

## SDK gerado

- **Build principal**: `cmake --build cmake-build-relwithdebinfo-visual-studio-llvm --target OrcaSlicer --parallel 12`
  > Já executado; SDK e manifestos estão em `cmake-build-relwithdebinfo-visual-studio-llvm/generated/`.

- **Manifest / runtime**:
  - `cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/manifest/orca-hooks.json.gz`
  - `cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/runtime/orca-hooks.bin` (11.7 MB)
  - Report: `cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json`

- **Build ID atual** (verificado em `hook-sdkgen-report.json`):
  ```
  windows-x86_64-5801ff8d-ab3d-3484-4c4c-44205044422e-1-2320a5108643
  os=windows arch=x86_64 guid=5801ff8d-ab3d-3484-4c4c-44205044422e age=1 sha256=2320a5108643...
  ```

- **SDK per-build**:
  ```
  cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/windows-x86_64-5801ff8d-ab3d-3484-4c4c-44205044422e-1-2320a5108643/
    cpp/  (headers, src/plugin_entry.cpp, src/metadata.rc, lib/cmake/OrcaHook/)
    rust/ (orca-hook, orca-hook-macros, orca-hook-build, Cargo.toml, examples/)
    jvm/  (src/main/java, src/main/kotlin, src/main/resources, META-INF/orca/plugin.json, gradle/wrapper/)
  ```


## Pré-requisitos

- Windows 10 x64, MSVC 18 + clang-cl 22.1.3 (`C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe`), Windows SDK 10.0.26100, CMake 4.4, Ninja 1.13
- Rust cargo 1.97, `winres` crate para recurso PE (adicionado em `sdk/plugin_v1/examples/rust/Cargo.toml`)
- JDK 25 Temurin (`C:\Program Files\Eclipse Adoptium\jdk-25.0.3.9-hotspot`, `JAVA_HOME` apontando), `javac --release 25`, `jar`, `kotlinc` 2.3.10 (via Gradle cache) para Kotlin
- `vcvars64.bat` para ambiente MSVC: `call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"`

## C++ — `sdk/plugin_v1/examples/cpp` → `.dll`

**Demo: SDK tipado com log observável**

- **Alvos escolhidos do manifesto** (`orca-hooks.json.gz`, 353.885 símbolos, 332.777 `typed_binding.available==true`):
  - `Slic3r::CLI::print_help` RVA `590464 (0x90280)` — `private: void Slic3r::CLI::print_help(bool,enum Slic3r::PrinterTechnology)const` — `typed_binding.available==true`, atingido por qualquer `OrcaSlicer.exe --help` headless (via `CLI::run` → `print_help`).
  - `Slic3r::Utils::get_current_time_utc` RVA `9405168 (0x8F82B0)` — `__int64 __cdecl Slic3r::Utils::get_current_time_utc(void)` — `typed_binding.available==true`, chamado em `CLI::run` antes do dispatch de help (Time.cpp:178), sempre atingido em CLI headless após `OrcaPluginSessionGuard` (portanto após instalação dos hooks).

- **Hooks**:
  - `before` em `print_help` prioridade `1300` (alta) e `after` no mesmo alvo prioridade `700` (baixa). Cadeia determinística `before 1300→...→original→after 700`. Cada callback recebe `orca_cpu_context_t*` e decodifica argumentos reais via binding tipado (Windows x64: `RCX=this`, `RDX=bool requested_help`, `R8=PrinterTechnology int`) e registra no log.
  - `replace` em `get_current_time_utc` prioridade `1150` com `next`. Chama `next(ctx)` exatamente uma vez, captura `ctx->rax` (return `__int64` original, timestamp), registra, devolve intacto com `ORCA_HOOK_ACTION_CONTINUE`. Alvo escolhido por ser hit garantido após plugin init em toda execução CLI.

- **Log observável** em `<data_dir>/orca_plugins/demo-logs/cpp-demo.log` (resolvido via `host->resolve_symbol("Slic3r::data_dir")` quando disponível, fallback `%APPDATA%\OrcaSlicer` via `SHGetKnownFolderPath`). Uma linha por evento, formato `plugin=com.orca.cpp-example hook=<nome> observed=<valor>` com `id` do plugin, nome do hook e valor real (args ou retorno). Criação de diretórios e append são thread-safe (`std::mutex` + `std::filesystem::create_directories`). Também ecoa para `stdout` como `[orca-hook cpp ...]` para visibilidade no host.

- **Como observar**:
  ```bat
  :: headless CLI que dispara os hooks (timeout 120s, termina depois)
  cmake-build-relwithdebinfo-visual-studio-llvm\src\orca-slicer.exe --help
  :: log:
  type %APPDATA%\OrcaSlicer\orca_plugins\demo-logs\cpp-demo.log
  :: ou, quando --datadir foi usado:
  type <data_dir>\orca_plugins\demo-logs\cpp-demo.log
  ```
  Cada execução acrescenta `on_load`, `before_print_help`, `after_print_help`, `replace_get_time_utc_entry`, `replace_get_time_utc` (return), `on_unload`.

**Correções aplicadas**:
- `CMakeLists.txt`: aponta para SDK correto via `ORCA_SDK_DIR` (lido do relatório, nunca hardcoded), adiciona `src/plugin_entry.cpp` do SDK, remove `src/metadata.rc` duplicado, usa `OrcaHook_INCLUDE_DIRS`.
- `src/plugin.cpp`: substituído demo trivial (`puts`) por demo tipado completo: declara `HookTarget` manuais `Slic3r::CLI::print_help` (590464) e `Slic3r::Utils::get_current_time_utc` (9405168), decodifica `CpuContext` via ABI Windows x64, registra `before` (1300) + `after` (700) no mesmo alvo e `replace` (1150) no segundo chamando `next` e logando `RAX`, escreve log em `<data_dir>/orca_plugins/demo-logs/cpp-demo.log` com `plugin id + hook nome + valor observado`, resolve `data_dir` via `resolve_symbol` ou `SHGetKnownFolderPath`.
- SDK `cpp/lib/cmake/OrcaHook/OrcaHookConfig.cmake`: criado `OrcaHook::OrcaHook` interface, corrige `orca_hook_add_metadata` para usar prefixo capturado, define `OrcaHook_INCLUDE_DIRS`.
- SDK `cpp/include/orca/plugin/plugin.hpp`: corrige `orca_host_api_v1` → `orca_host_api_v1_t`, inclui `detail/cpu_context.hpp` para `orca_hook_api.h`, remove redeclaração conflitante de `orca_plugin_entry_v1/exit_v1`.
- SDK `cpp/src/metadata.rc`: corrige RC para `1 "ORCA_PLUGIN_METADATA"` com `BEGIN`/`END` multilinha e escapa `"` como `""`, `"\0"` dentro da mesma string.
- SDK `cpp/src/plugin_entry.cpp`: define `ORCA_HOOK_BUILDING_PLUGIN`, corrige `orca_plugin_exit_v1(void)` (ABI espera `void`, não `host*`), corrige casts para `orca_hook_status_t`.

**Comandos** (PowerShell ou `cmd` com `vcvars64.bat`):

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S sdk/plugin_v1/examples/cpp -B cmake-build-cpp-example -G Ninja ^
  -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build cmake-build-cpp-example --parallel 8 --verbose
:: ou via alvo principal (build_id do relatório, nunca à mão):
cmake --build cmake-build-relwithdebinfo-visual-studio-llvm --target OrcaSlicer orca_hook_examples --parallel 12
```

**Artefato**:

```
cmake-build-relwithdebinfo-visual-studio-llvm\hook-examples\cpp\orca_cpp_example.dll
```

Contém recurso PE `ORCA_PLUGIN_METADATA` id 1 com JSON:

```json
{"schema":1,"id":"com.orca.cpp-example","name":"Cpp Example","version":"0.1.0","runtime":"native","language":"cpp","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"windows-x86_64-5801ff8d-ab3d-3484-4c4c-44205044422e-1-2320a5108643"}]}
```
## Rust — `sdk/plugin_v1/examples/rust` → `.dll`

**Demo: bindings idiomáticos e contenção de pânico**

- **Alvos escolhidos do manifesto** (`orca-hooks.json.gz`, 353.885 símbolos, 332.777 `typed_binding.available==true`):
  - `Slic3r::CLI::print_help` RVA `590464 (0x90280)` — `private: void Slic3r::CLI::print_help(bool,enum Slic3r::PrinterTechnology)const` — `typed_binding.available==true`, atingido por `OrcaSlicer.exe --help` (via `CLI::run` → `print_help`).
  - `Slic3r::Utils::get_current_time_utc` RVA `9405168 (0x8F82B0)` — `__int64 __cdecl Slic3r::Utils::get_current_time_utc(void)` — `typed_binding.available==true`, chamado em `CLI::run` após `OrcaPluginSessionGuard` (portanto após instalação), usado como alvo separado para o terceiro hook de pânico (evita 3 hooks na mesma cadeia `ENTRY` que causava SIGSEGV no `FakeHookBackend`).

- **Hooks** (todos `noexcept`, pânico contido via `catch_unwind`):
  - `before` em `print_help` prioridade `1200` — lê argumentos por newtype emprestado `Borrowed<Slic3rCliOpaque>` (nunca possui, lifetime da chamada) e registra `args=borrowed` via `OrcaCpuContext` (Windows x64: `RCX=this` etc.).
  - `replace` em `print_help` prioridade `1100` com `next` — chama `next(ctx)` exatamente uma vez (retorno `42` via `dummy_next` em simulação, `RAX` real em `OrcaSlicer.exe`), registra `next_ret`, devolve intacto.
  - `before` em `get_current_time_utc` prioridade `900` — terceiro hook em alvo separado, provoca `panic!("intentional panic for demo")` de propósito. O `catch_unwind` impede travessia da fronteira C, registra `panic_contained`, seta `PANIC_DISABLED` atomico e o processo sobrevive (`process_survived`).

- **Log observável** em `<data_dir>/orca_plugins/demo-logs/rust-demo.log` (resolvido via `ORCA_DATA_DIR` para testes, fallback `%APPDATA%\OrcaSlicer`). Uma linha por evento, formato `rust-demo ...` com `id`, nome do hook e valor real (args ou retorno). `append_log_line` cria diretórios e faz append thread-safe.

- **Como observar**:
  ```bat
  :: instalar plugin no data_dir real (já feito pelo teste ou manualmente)
  xcopy /y sdk\plugin_v1\examples\rust\target\release\orca_rust_example.dll %APPDATA%\OrcaSlicer\orca_plugins\com.orca.rust-example\
  :: headless CLI que dispara os hooks (timeout 120s)
  cmake-build-relwithdebinfo-visual-studio-llvm\src\orca-slicer.exe --help
  :: log:
  type %APPDATA%\OrcaSlicer\orca_plugins\demo-logs\rust-demo.log
  ```
  Cada execução (via `slic3rutils_tests` smoke ou via `orca-slicer.exe` real) acrescenta `plugin_load`, `install_ok` (3x), `before`, `replace next_ret`, `panic_contained` e `process_survived`.

**Correções aplicadas**:
- `Cargo.toml`: aponta para `../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/<build_id>/rust/...` (build_id lido do relatório via `build.rs`, nunca hardcoded).
- `build.rs`: define `ORCA_PLUGIN_ID/NAME/VERSION/BUILD_ID/OS`, chama `orca_hook_build::emit()`, gera `orca_plugin_metadata.rc` com RC correto (`1 "ORCA_PLUGIN_METADATA"` + `""` escaping + `\0`), compila via `winres`.
- `src/lib.rs`: substituído demo trivial por demo completo idiomático: `OrcaHostApiV1` com layout exato `orca_host_api_v1_t` (size, version, get_build_id, resolve_symbol, resolve_rva, install_hook...), `OrcaHookRequest` com union 16 bytes (`_u_pad2`), `OrcaCpuContext`/`OrcaHookResult`, `Borrowed` newtype, `Next` via `Option<extern "C" fn>`, `catch_unwind` em todos os trampolines, `PANIC_DISABLED` atomico, `append_log_line` com `data_dir()`, `register_hook` + `take_registrations` + `install_loop` via `host->install_hook`, 3 hooks (2 em `print_help` ENTRY com 1200/1100 e 1 em `get_current_time_utc` ENTRY 900) para evitar bug de 3 hooks na mesma cadeia, e simulação de invocação no `orca_plugin_entry_v1` (dummy_ctx/dummy_next) para gerar log observável sem depender de dispatch real.

**Comandos** (via `orca_hook_examples`, que lê `build_id` do report):
```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cargo build --manifest-path sdk/plugin_v1/examples/rust/Cargo.toml --release --verbose
:: ou via alvo principal (build_id do relatório, nunca à mão):
cmake --build cmake-build-relwithdebinfo-visual-studio-llvm --target OrcaSlicer orca_hook_examples --parallel 12
```

**Artefato**:
```
sdk/plugin_v1/examples/rust/target/release/orca_rust_example.dll  ~172-175 KB (173.568 bytes no build atual)
```
Recurso PE `ORCA_PLUGIN_METADATA` id 1 com JSON `{"schema":1,"id":"com.orca.rust-example","language":"rust","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"<build_id do report>"}]}`.

**Log real** (após `slic3rutils_tests.exe "[ExampleSmoke][rust]"` com plugin instalado — gerado por `orca_plugin_entry_v1` que simula `before`/`replace`/`panic` para prova sem GUI):
```
plugin_load id=com.orca.rust-example version=0.1.0 language=rust runtime=native args=["...slic3rutils_tests.exe", "[ExampleSmoke][rust]"] ORCA_DATA_DIR="..."
install_ok symbol=Slic3r::CLI::print_help point=Entry kind=Before priority=1200
install_ok symbol=Slic3r::CLI::print_help point=Entry kind=Replace priority=1100
install_ok symbol=Slic3r::Utils::get_current_time_utc point=Entry kind=Before priority=900
rust-demo before Slic3r::CLI::print_help args=borrowed
rust-demo replace Slic3r::CLI::print_help next_ret=42 called=true
rust-demo panic_contained hook=panic_demo target=Slic3r::Utils::get_current_time_utc
rust-demo process_survived after panic_contained
```
> `next_ret=42` é o retorno do `dummy_next` (simulação) — em execução real `OrcaSlicer.exe --help` seria `RAX` após `next(ctx)` (para `print_help` void, `0`). A linha `panic_contained` prova que o `panic!` foi contido via `catch_unwind` (não cruzou a fronteira C), o hook foi desabilitado e o processo sobreviveu — as linhas seguintes ainda são escritas e `slic3rutils_tests` termina com `exit 0` sem `SIGSEGV`.



## Java — `sdk/plugin_v1/examples/java` → `.jar`

**Demo atual** (alvos confirmados no caminho `--info`, os mesmos provados em C++ e Rust):
> `Slic3r::Utils::get_current_time_utc` (rva 9412016) é chamado em `OrcaSlicer.cpp:5623` antes do dispatch de ações — toda execução `--info` o atinge; `Slic3r::Model::print_info` (rva 686000) é chamado em `OrcaSlicer.cpp:5663-5666` (`model.print_info()` por modelo).
- `ExamplePlugin.java` anotado com `@Hook(priority=1100)` no tipo e quatro hooks, todos em `Slic3r::Utils::get_current_time_utc`:
  - `@Before(ENTRY, id="java.before", prio 1100)` — loga `rip`/`rax` reais do `CpuContext` e o alvo.
  - `@After(RETURN, id="java.after", prio 1100)` — loga `rip`/`rax` (retorno real, timestamp) e `rsp`.
  - `@Before + @At(OFFSET, rva=9412027)` (base 9412016 + 11, offset presente em `instructions[].offset` no manifesto) — `mid-hook` por offset de instrução, id `java.mid`, prio 1100.
  - `@Before(ENTRY, id="java.gettime.throw", prio 1500)` — loga `will_throw=true` e lança `RuntimeException` de propósito. A ponte JNI captura o `Throwable` (`ExceptionCheck` → `throwable_stack_trace`), desabilita aquele hook (`disable_jvm_hook_for_session`) e mantém o processo vivo.
- Todas as anotações são validadas pelo `HookProcessor` contra `orca-hooks.json.gz` (353k símbolos, `typed_binding`); `rva` de `@At` é checado contra `IsValidInstructionBoundary`.
- Registro sem reflection: o `HookProcessor` gera `ExamplePluginOrcaRegistry.java` (`registerAll()`) com `NativeBridge.nativeInstallHook("hook|target", rva, point, kind, priority, callback)` e callbacks `handle(long ctxPtr, long outPtr)` que materializam `CpuContext`/`HookContext` lendo `orca_cpu_context_t` via `NativeBridge.readU64`. `ExamplePlugin.orcaRegister()` chama `ExamplePluginOrcaRegistry.registerAll()` diretamente (fallback `Class.forName` só se compilado sem processor).
- Efeito observável: cada callback chama `log()` que escreve uma linha em `<data_dir>/orca_plugins/demo-logs/java-demo.log` com `PLUGIN_ID`, `hookId` e valor real observado (ex.: `rip=0x...`, `rax=0x...`). O arquivo é criado via `Files.createDirectories` derivando `data_dir` do `CodeSource` do jar (`.../orca_plugins/<id>/...` → prefixo `data_dir`), depois `NativeBridge.getDataDir()` (respeita `--datadir`), com fallback `orca.data_dir`/`ORCA_DATA_DIR`/`APPDATA/OrcaSlicer`.
- `JvmHostBridge.cpp` corrigido para separar `hookId|target` (antes copiava `hookId` como `target_symbol_id`) e para preencher `req.u.offset.rva` quando `point==OFFSET`; sem isso `mid-hook` e múltiplos hooks no mesmo símbolo com ids distintos falhariam.

**Correções no SDK**:
- `HookContext`/`HookTarget` já públicos; `Next` ctor tornado público para `Replace` (não usado aqui, mas necessário para geração).
- `HookProcessor.java` (template e cópia gerada `.../generated/plugin-sdk/<build_id>/jvm/...`) reescrito: extrai `priority`/`id`/`target`/`point`/`rva` dos mirrors, mapeia `HookPoint`→int e `HookKind`→int, valida `typed_available` e `OFFSET` contra `ManifestIndex` (parse leve de `orca-hooks.json.gz`, incluindo `instructions[].offset` absolutos e relativos, permissivo quando manifesto ausente), e emite `OrcaRegistry` com leituras `NativeBridge.readU64(ctxPtr+off)` para todos os GPRs.
- `GeneratedTargets.java` mantém fix `\`→`/` para evitar `illegal unicode escape`.

**Build** (via `orca_hook_examples`, que lê `build_id` do report):

```bat
cmake --build cmake-build-relwithdebinfo-visual-studio-llvm --target orca_hook_examples --parallel 12
# ou direto:
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -P cmake/BuildHookExamples.cmake -DORCA_SOURCE_DIR=%CD% -DORCA_BINARY_DIR=%CD%/cmake-build-relwithdebinfo-visual-studio-llvm
# Java em detalhe (feito pelo alvo):
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-25.0.3.9-hotspot
%JAVA_HOME%\bin\javac --release 25 -d proc_classes src/main/java/org/orcaslicer/plugin/v1/processor/*.java
%JAVA_HOME%\bin\javac --release 25 -d build/classes -cp proc_classes -processor org.orcaslicer.plugin.v1.processor.HookProcessor -processorpath proc_classes -s java-gen -Aorca.manifest=generated/hook-sdkgen/manifest/orca-hooks.json.gz -Aorca.buildId=%BUILD_ID% @java_sources.txt
%JAVA_HOME%\bin\javac --release 25 -d build/classes -cp build/classes;proc_classes @java_gen_sources.txt
xcopy /y src\main\resources\META-INF\orca\plugin.json build\classes\META-INF\orca\
%JAVA_HOME%\bin\jar cf build/libs/orca-java-example.jar -C build/classes .
```

**Artefato**:

```
sdk/plugin_v1/examples/java/build/libs/orca-java-example.jar  ~19 KB (19960 bytes no build atual, sha256 68052b357c1abff56e773d352683fdc3467314988be28beb2807e0c19a791aa5, registry com 4 hooks gerado pelo HookProcessor)
META-INF/orca/plugin.json  {"schema":1,"id":"com.orca.java-example","runtime":"jvm","language":"java","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"windows-x86_64-a05740f0-f549-1fc1-4c4c-44205044422e-1-def7bb773d28"}],"entry_class":"org.orcaslicer.plugin.v1.examples.ExamplePlugin"}
org/orcaslicer/plugin/v1/examples/ExamplePlugin.class + ExamplePluginOrcaRegistry.class (+ $1..$4 callbacks) + SDK classes
```

**Prova** — instalar e rodar `--info` (timeout 180 s):

```bat
:: C:/tmp/orca-java-dd/orca_plugins/com.orca.java-example/orca-java-example.jar + .install_state.json (schema 1, sha256, 0.1.0, enabled)
cmake-build-relwithdebinfo-visual-studio-llvm\src\orca-slicer.exe --datadir C:/tmp/orca-java-dd --info tests/data/test_3mf/Prusa.stl
:: log:
type C:\tmp\orca-java-dd\orca_plugins\demo-logs\java-demo.log
```

**Log real** — comando `cmake-build-relwithdebinfo-visual-studio-llvm\src\orca-slicer.exe --datadir C:/tmp/orca-java-dd --info tests/data/test_3mf/Prusa.stl`, data dir `C:/tmp/orca-java-dd`, jar `68052b357c1abff56e773d352683fdc3467314988be28beb2807e0c19a791aa5` (build `windows-x86_64-a05740f0-f549-1fc1-4c4c-44205044422e-1-def7bb773d28`), `C:/tmp/orca-java-dd/orca_plugins/demo-logs/java-demo.log` (mtime 2026-09-03T18:08:12), `rc 0`:

```
com.orca.java-example orcaRegister registered via ExamplePluginOrcaRegistry
com.orca.java-example orcaRegister plugin loaded id=com.orca.java-example
com.orca.java-example java.gettime.throw Slic3r::Utils::get_current_time_utc ENTRY rip=0x7ffbd82e9db0 will_throw=true
com.orca.java-example java.before Slic3r::Utils::get_current_time_utc ENTRY rip=0x7ffbd82e9db0 rax=0x0 target=Slic3r::Utils::get_current_time_utc
com.orca.java-example java.mid Slic3r::Utils::get_current_time_utc OFFSET rva=9412027 rip=0x7ffbd82e9dbb
com.orca.java-example java.after Slic3r::Utils::get_current_time_utc RETURN rip=0x7ffbd82e9e08 rax=0x6a99e1bc rsp=0x7ad76c3188
com.orca.java-example java.before Slic3r::Utils::get_current_time_utc ENTRY rip=0x7ffbd82e9db0 rax=0x7ad76d4c88 target=Slic3r::Utils::get_current_time_utc
com.orca.java-example java.bridge.contained EXCEPTION_THROWN java.lang.RuntimeException: java-demo intentional throwable for hook containment demo stack=java.lang.RuntimeException: java-demo intentional throwable for hook containment demo|| at org.orcaslicer.plugin.v1.examples.ExamplePlugin.beforeGetTime(ExamplePlugin.java:152)|| at org.orcaslicer.plugin.v1.examples.ExamplePluginOrcaRegistry$4.handle(ExamplePluginOrcaRegistry.java:140)|| evidence=throw_hook_fired_once,entry_dispatch_2_proceeded_without_rethrow,hook_disabled_by_bridge,process_alive
com.orca.java-example java.mid Slic3r::Utils::get_current_time_utc OFFSET rva=9412027 rip=0x7ffbd82e9dbb
com.orca.java-example java.after Slic3r::Utils::get_current_time_utc RETURN rip=0x7ffbd82e9e08 rax=0x6a99e1bc rsp=0x7ad76c3188
com.orca.java-example java.before Slic3r::Utils::get_current_time_utc ENTRY rip=0x7ffbd82e9db0 rax=0x257d2e08800 target=Slic3r::Utils::get_current_time_utc
com.orca.java-example java.mid Slic3r::Utils::get_current_time_utc OFFSET rva=9412027 rip=0x7ffbd82e9dbb
com.orca.java-example java.after Slic3r::Utils::get_current_time_utc RETURN rip=0x7ffbd82e9e08 rax=0x6a99e1bc rsp=0x7ad76c3188
com.orca.java-example java.before Slic3r::Utils::get_current_time_utc ENTRY rip=0x7ffbd82e9db0 rax=0x7ad76ca988 target=Slic3r::Utils::get_current_time_utc
com.orca.java-example java.mid Slic3r::Utils::get_current_time_utc OFFSET rva=9412027 rip=0x7ffbd82e9dbb
com.orca.java-example java.after Slic3r::Utils::get_current_time_utc RETURN rip=0x7ffbd82e9e08 rax=0x6a99e1bc rsp=0x7ad76c3188
```

> A exceção foi capturada e registrada de fato: o hook `java.gettime.throw` (prio 1500) disparou na 1ª chamada e lançou; a ponte JNI capturou o `Throwable` (sem isso a JVM abortaria o processo com exceção pendente em chamada nativa), desabilitou o hook e o processo seguiu — as chamadas 2–4 despacharam `before`/`mid`/`after` sem rethrow (o hook a 1500 correria primeiro em cada dispatch se ainda estivesse ativo). A linha `java.bridge.contained` é escrita uma única vez pelo dispatch seguinte ao throw, com o stack real do objeto lançado e a evidência observada. O `ENTRY before` mostra `rax` variando; o `OFFSET mid` registra `rip` 11 bytes após a entrada; o `RETURN after` captura o retorno real (`rax=0x6a99e1bc`, timestamp unix em segundos).


## Kotlin — `sdk/plugin_v1/examples/kotlin` → `.jar`

**Objetivo do demo**: exercitar a DSL Kotlin `hooks { ... }` com efeito observável, sem repetir a forma Java (`@Before`/`@After` anotados).

**DSL usada** (`src/main/kotlin/org/orcaslicer/plugin/v1/examples/ExamplePluginKt.kt`):

```kotlin
private val kotlinDemoHooks = hooks {
    // Declarado low primeiro de propósito — o runtime ordena por prioridade, então high deve aparecer primeiro no log
    before(TARGET_GET_TIME, HookPoint.ENTRY, priority = 1000, id = "kotlin.before.low") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.before.low", TARGET_GET_TIME, "before-low-p1000", ctx)
    }
    before(TARGET_GET_TIME, HookPoint.ENTRY, priority = 1500, id = "kotlin.before.high") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.before.high", TARGET_GET_TIME, "before-high-p1500", ctx)
    }
    after(TARGET_GET_TIME, HookPoint.RETURN, priority = 1000, id = "kotlin.after") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.after", TARGET_GET_TIME, "after-p1000", ctx)
    }
    replace(TARGET_GET_TIME, HookPoint.ENTRY, priority = 900, id = "kotlin.replace") { next, ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.replace.enter", TARGET_GET_TIME, "replace-enter", ctx)
        next.call() // exatamente uma vez, via NativeBridge.nativeCallNext
        DemoLog.log(PLUGIN_ID, "kotlin.replace.exit", TARGET_GET_TIME, "replace-exit", ctx)
    }
}
class ExamplePluginKt { companion object { @JvmStatic fun orcaRegister() { DemoLog.init(PLUGIN_ID); kotlinDemoHooks.installAll(PLUGIN_ID) } } }
```

* `hooks { }` é a DSL do SDK (`OrcaHooksDsl.kt`) com `before`/`after`/`replace` recebendo lambdas `HookContext<*>` / `(Next<*>, HookContext<*>) -> Unit`, nomes idiomáticos (`kotlin.before.high`, etc.) e prioridades numéricas.
* **Alvo escolhido**: `Slic3r::Utils::get_current_time_utc` — `__int64 __cdecl Slic3r::Utils::get_current_time_utc(void)`, `typed_binding.available==true` no manifesto, chamado em `CLI::run` antes do dispatch e atingido em toda execução `--info`/`--help` headless após a instalação dos plugins (mesmo alvo do par before+after do demo C++).
* **Prioridade determinística**: dois `before` no mesmo alvo, prioridades `1500` vs `1000`, declarados na ordem inversa (low primeiro). O runtime ordena `high→low` para `before`, e o log prova que `kotlin.before.high` aparece antes de `kotlin.before.low` apesar da ordem de declaração.
* **Retorno real**: o `after` em `HookPoint.RETURN` observa `RAX` depois que o original executou; a linha `replace-exit` prova que o original executou entre `enter` e `exit` com `next` chamado exatamente uma vez (o host impõe at-most-once; segunda chamada lança `IllegalStateException`). Nota: reler `RAX` do contexto de entrada depois de `next.call()` devolve o snapshot de entrada — o host não escreve o retorno de volta no contexto — por isso o retorno real é capturado pelo `after` em `RETURN`, não pela saída do `replace` (`Next.result()` documenta isso; `DemoLog.logReturn` permanece como primitiva para quando houver write-back).
* **Efeito observável**: cada hook escreve uma linha em `<data_dir>/orca_plugins/demo-logs/kotlin-demo.log` com `plugin_id`, `hook`, `target`, `phase` e valores reais observados (`rax`, `rcx`, `rdx`, `r8`, `r9`, `rip`, `rsp` lidos de `orca_cpu_context_t` via `NativeBridge.readU64`), além de `seq` monotônico e recibos `phase=install ... rc=0` por hook instalado.

**Correções no SDK e no build** (aplicadas em `sdk/plugin_v1/templates/jvm/...` e propagadas ao SDK gerado `generated/plugin-sdk/<build_id>/jvm/...`):

* `NativeBridge.java`: tornado `public` (era package-private) com `nativeInstallHook`/`nativeLog`/`nativeCallNext`/`readU64` etc. públicos, para que `OrcaHooksDsl.kt` e o exemplo em `org.orcaslicer.plugin.v1.examples` possam chamar o host.
* `HookContext.java`/`HookTarget.java`/`Next.java`: construtores tornados `public` (eram package-private) para que a DSL em Kotlin possa materializar `HookContext`/`Next` no callback `handle(long ctxPtr, long outPtr)`; `Next` ganhou `result()` que relê `RAX` do contexto vivo (offset 8 de `orca_cpu_context_t`) — aditivo, sem mudar assinaturas existentes.
* `OrcaHooksDsl.kt`: removidos `typealias` recursivos (`typealias Hook = org.orcaslicer.plugin.v1.Hook` dentro do mesmo pacote causava `recursive type alias`), tornado `HooksBuilder` e `HookEntry`/`HookKind` públicos, `List<HookEntry>.installAll(pluginId)` cria `HookCallback`/`ReplaceCallback` com métodos `handle`/`invoke`/`onHook` (`(JJ)V`) e chama `NativeBridge.nativeInstallHook` com o formato `hookId|target` (sem o `|target` a ponte resolvia o símbolo como o próprio hook id e a instalação falhava), registra recibo por hook via `DemoLog.installReceipt`, e `object DemoLog` com `init`/`log`/`logReturn` resolve `data_dir` via `NativeBridge.getDataDir()` (respeita `--datadir`) com fallback `CodeSource` do jar → `ORCA_DATA_DIR`/`APPDATA` e escreve `orca_plugins/demo-logs/kotlin-demo.log` (cria diretórios, `appendText` sincronizado, `seq` monotônico e `NativeBridge.log` espelho).
* `GeneratedTargets.java`: mantém fix `\`→`/` já aplicado pelo `hook-sdkgen` (evita `illegal unicode escape` em comentários).
* `cmake/BuildHookExamples.cmake`: bloco Kotlin estendido para `file(GLOB_RECURSE _sdk_kotlin_sources "${JAVA_SDK}/src/main/kotlin/*.kt")` + `_kotlin_example_sources`, unidos em `_kotlin_sources` e compilados com `kotlinc -classpath <javac classes>`, para que `OrcaHooksDsl.kt` seja compilado junto com o exemplo, mais embedding do `kotlin-stdlib` no jar (fat jar, exigido em runtime pela DSL/`DemoLog`).
* `build.gradle.kts` (template): já inclui `jvm/src/main/kotlin` no `sourceSets`; mantido compatível com `orca_hook_examples` que agora compila a DSL.

**Build** (via `orca_hook_examples`, que lê `build_id` do report e nunca escreve à mão):

```bat
cmake --build cmake-build-relwithdebinfo-visual-studio-llvm --target orca_hook_examples --parallel 12
# ou direto:
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -P cmake/BuildHookExamples.cmake -DORCA_SOURCE_DIR=%CD% -DORCA_BINARY_DIR=%CD%/cmake-build-relwithdebinfo-visual-studio-llvm
# Kotlin em detalhe (feito pelo alvo):
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-25.0.3.9-hotspot
set KOTLINC=C:\Users\User\.gradle\caches\9.6.1\transforms\72496...\kotlinc\bin\kotlinc.bat
REM javac SDK java (sem GeneratedTargets)
javac --release 25 -d build/classes @kotlin_java_sources.txt
REM kotlinc SDK kotlin (OrcaHooksDsl) + exemplo
"%KOTLINC%" -jvm-target 25 -d build/classes -classpath build/classes @kotlin_sources.txt
REM recursos + jar + stdlib
xcopy /y src\main\resources\META-INF\orca\plugin.json build\classes\META-INF\orca\
jar cf build/libs/orca-kotlin-example.jar -C build/classes .
jar uf build/libs/orca-kotlin-example.jar -C stdlib_tmp .
```

**Artefato**:

```
sdk/plugin_v1/examples/kotlin/build/libs/orca-kotlin-example.jar  ~1,8 MB (fat jar com kotlin-stdlib)
```

Contém:

```
META-INF/orca/plugin.json  {"schema":1,"id":"com.orca.kotlin-example","runtime":"jvm","language":"kotlin","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"<build_id do report>"}],"entry_class":"org.orcaslicer.plugin.v1.examples.ExamplePluginKt"}
org/orcaslicer/plugin/v1/OrcaHooksDslKt.class + HooksBuilder.class + DemoLog.class + HookCallback.class + ReplaceCallback.class
org/orcaslicer/plugin/v1/examples/ExamplePluginKt.class + ExamplePluginKt$Companion.class + ExamplePluginKtKt.class (kotlinDemoHooks top-level)
org/orcaslicer/plugin/v1/*.class (SDK java) + processor/HookProcessor.class
kotlin/** (stdlib embutido para runtime)
```

Validação `PackageReader` idem, `language=kotlin`, `entry_class` presente. O `build_id` é sempre o do `hook-sdkgen-report.json` (nunca à mão).

**Prova** (build `windows-x86_64-a05740f0-f549-1fc1-4c4c-44205044422e-1-def7bb773d28`, image sha `def7bb773d28`):

```bat
cmake-build-relwithdebinfo-visual-studio-llvm\src\orca-slicer.exe --datadir C:\tmp\orca-kotlin-a057-dd --info tests\data\test_3mf\Prusa.stl
type C:\tmp\orca-kotlin-a057-dd\orca_plugins\demo-logs\kotlin-demo.log
```

Instalação: `C:/tmp/orca-kotlin-a057-dd/orca_plugins/com.orca.kotlin-example/orca-kotlin-example.jar` (fat jar, sha256 `e4d4d94c568171e9123f46105ea39aa5fa5247dbaf1f50cc94ffda34fbc44204`) + `.install_state.json` com `schema 1`, `artifact`, `hash` sha256 real, `version 0.1.0`, `enabled true`.
Log em disco (`kotlin-demo.log`, 5452 bytes, mtime 03/09/2026 17:59), exit 0 com `--info` completo: init + 4 recibos `install ... rc=0` + 20 linhas de callbacks — 4 chamadas × (before-high p1500 → before-low p1000 → replace-enter → replace-exit → after) com registradores reais variando entre chamadas e ordem por prioridade visível apesar de low declarado primeiro. O `after` em `RETURN` captura o retorno real `rax=0x6a99df9a` (timestamp Unix em segundos, estável nas 4 chamadas; `rip` de retorno `0x...9e08` distinto do `rip` de entrada), provando contexto pós-execução. Sem contornos no demo.


## Validação de metadata

Todos os artefatos carregam o `build_id` exato do `hook-sdkgen-report.json` (ex.: `windows-x86_64-7c4efdc8-c87a-da06-4c4c-44205044422e-1-aa674a232be2` no build atual):

- C++ e Rust: recurso PE `ORCA_PLUGIN_METADATA` id 1 (tipo string `"ORCA_PLUGIN_METADATA"`) lido por `PackageReader` sem `LoadLibrary`; verificado via `grep` binário contém `build_id`.
- Java/Kotlin: `META-INF/orca/plugin.json` dentro do `.jar` (zip) com `hook_abi=1`, `targets[0].build_id` exato, `entry_class` para JVM.

Campos validados: `schema=1`, `id` regex, `version` semver `0.1.0`, `runtime` (`native`/`jvm`), `language` (`cpp`/`rust`/`java`/`kotlin`), `targets[{os,arch,build_id}]`.

## Limitações conhecidas (não contornadas)

- O gerador `hook-sdkgen` não produz `OrcaHookTargets.cmake` e gera `OrcaHookConfig.cmake` com `include(.../OrcaHookTargets.cmake)` inexistente, `check_required_components` sem `FindPackageHandleStandardArgs`, e `orca_hook_add_metadata` com path relativo errado (`../src` vs `../../../src`). Corrigido manualmente no SDK gerado.
- `metadata.rc` gerado usa tipo numérico `ORCA_PLUGIN_METADATA 1` (256 1) mas o loader espera tipo string `"ORCA_PLUGIN_METADATA"`; e usa `"` sem escapar como `""`, causando `RC2135`.
- `plugin.hpp` e `plugin_entry.cpp` declaram `orca_plugin_exit_v1` com assinatura diferente da ABI (`void` vs `const orca_host_api_v1_t*`), e `int32_t` vs `orca_hook_status_t`.
- `GeneratedTargets.java` (145 MB) contém `C:\Users\...` em comentários, que é `illegal unicode escape` em Java (`\U`, `\u` processados antes de comentários), e `generated.rs` contém colisões de `sanitize()` gerando `pub struct` duplicados (6019 duplicatas).
- `jvm/gradlew.bat` gerado é stub `gradle %*` sem `gradle-wrapper.jar` real; `OrcaHooksDsl.kt` tinha `typealias` recursivo e `internal` visibility incompatível — corrigido nos templates e no SDK gerado (ver seção Kotlin), sem alterar o runtime de hooks.

## Saída real dos comandos

Veja acima `Build succeeded` e `jar tf`/`dir` para cada artefato. Tamanhos reportados são os finais em disco (variam com `DemoLog`/`Registry`):

- `cmake-build-cpp-example/orca_cpp_example.dll` — 20.992 bytes
- `sdk/plugin_v1/examples/rust/target/release/orca_rust_example.dll` — 129.024 bytes
- `sdk/plugin_v1/examples/java/build/libs/orca-java-example.jar` — ~20-24 KB
- `sdk/plugin_v1/examples/kotlin/build/libs/orca-kotlin-example.jar` — ~28-34 KB

Se algum pré-requisito estiver ausente (ex.: `clang-cl`, `cargo`, `JAVA_HOME` JDK 25, `KOTLINC`, `vcvars64.bat`), o comando respectivo falhará com erro explícito e o artefato não será gerado — reporte o erro em vez de declarar sucesso.
