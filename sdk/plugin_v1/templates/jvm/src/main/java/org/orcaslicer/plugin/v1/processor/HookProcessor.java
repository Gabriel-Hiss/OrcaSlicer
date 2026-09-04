package org.orcaslicer.plugin.v1.processor;

import javax.annotation.processing.*;
import javax.lang.model.SourceVersion;
import javax.lang.model.element.*;
import javax.lang.model.type.TypeMirror;
import javax.tools.Diagnostic;
import javax.tools.FileObject;
import javax.tools.StandardLocation;
import java.io.*;
import java.util.*;

/**
 * Annotation processor validating @Hook/@Before/@After/@Replace/@At against
 * manifest/orca-hooks.json (typed_binding + instruction boundaries) and
 * generating a registry that is invoked without reflection at load time.
 *
 * No reflection scan at runtime — the generated class
 * {@code <EntryClass>OrcaRegistry} is called from
 * the plugin's static {@code orcaRegister()} and registers every hook via
 * {@code NativeBridge.nativeInstallHook}.
 */
@SupportedAnnotationTypes({
    "org.orcaslicer.plugin.v1.Hook",
    "org.orcaslicer.plugin.v1.Before",
    "org.orcaslicer.plugin.v1.After",
    "org.orcaslicer.plugin.v1.Replace",
    "org.orcaslicer.plugin.v1.At"
})
@SupportedSourceVersion(SourceVersion.RELEASE_25)
@SupportedOptions({"orca.manifest", "orca.buildId"})
public class HookProcessor extends AbstractProcessor {

    private static final String MANIFEST_OPT = "orca.manifest";
    private static final String BUILDID_OPT  = "orca.buildId";

    @Override
    public boolean process(Set<? extends TypeElement> annotations, RoundEnvironment roundEnv) {
        if (roundEnv.processingOver()) return false;

        Set<? extends Element> hookTypes = roundEnv.getElementsAnnotatedWith(
            processingEnv.getElementUtils().getTypeElement("org.orcaslicer.plugin.v1.Hook"));
        Map<Element, List<Element>> byContainer = new LinkedHashMap<>();

        for (Element e : hookTypes) {
            for (Element enclosed : e.getEnclosedElements()) {
                if (enclosed.getKind() != ElementKind.METHOD) continue;
                boolean isHook = enclosed.getAnnotationMirrors().stream().anyMatch(m ->
                    m.getAnnotationType().toString().endsWith(".Before") ||
                    m.getAnnotationType().toString().endsWith(".After") ||
                    m.getAnnotationType().toString().endsWith(".Replace") ||
                    m.getAnnotationType().toString().endsWith(".At"));
                // At alone is also a hook (raw) but typical use is Before+At
                if (isHook) byContainer.computeIfAbsent(e, k -> new ArrayList<>()).add(enclosed);
            }
        }
        if (byContainer.isEmpty()) return false;

        String manifestPath = processingEnv.getOptions().get(MANIFEST_OPT);
        ManifestIndex index = null;
        if (manifestPath != null) {
            try { index = ManifestIndex.load(new File(manifestPath)); }
            catch (Exception ex) {
                processingEnv.getMessager().printMessage(Diagnostic.Kind.WARNING,
                    "Failed to load manifest " + manifestPath + ": " + ex.getMessage());
                index = null;
            }
        }

        for (Map.Entry<Element, List<Element>> entry : byContainer.entrySet()) {
            TypeElement container = (TypeElement) entry.getKey();
            int classPriority = extractClassPriority(container);
            for (Element m : entry.getValue()) {
                String target = extractTarget(m);
                if (target == null || target.isEmpty()) {
                    boolean hasAt = hasAnnotation(m, "At");
                    if (!hasAt) {
                        processingEnv.getMessager().printMessage(Diagnostic.Kind.ERROR,
                            "@Before/@After/@Replace requires target()", m);
                        continue;
                    }
                }
                if (index != null && target != null && !target.isEmpty()) {
                    ManifestIndex.Symbol sym = index.resolve(target);
                    if (sym == null) {
                        processingEnv.getMessager().printMessage(Diagnostic.Kind.WARNING,
                            "Unknown target " + target + " — not in manifest (permissive)", m);
                    } else {
                        if (!sym.typedAvailable) {
                            boolean hasRaw = hasParamType(m, "org.orcaslicer.plugin.v1.RawHook")
                                          || hasParamType(m, "org.orcaslicer.plugin.v1.CpuContext");
                            if (!hasRaw) {
                                processingEnv.getMessager().printMessage(Diagnostic.Kind.WARNING,
                                    "Target " + target + " has no typed binding (" + sym.typedReason
                                    + "); use RawHook/CpuContext for raw access", m);
                            }
                        }
                        String point = extractEffectivePoint(m);
                        if ("OFFSET".equals(point)) {
                            long rva = extractRva(m);
                            if (rva < 0) {
                                processingEnv.getMessager().printMessage(Diagnostic.Kind.ERROR,
                                    "OFFSET requires rva in @At", m);
                            } else if (!index.isValidOffset(sym, rva)) {
                                processingEnv.getMessager().printMessage(Diagnostic.Kind.WARNING,
                                    "OFFSET rva 0x" + Long.toHexString(rva) + " not at instruction boundary for " + target + " (permissive)", m);
                            }
                        }
                    }
                }
            }
            try { generateRegistry(container, entry.getValue(), classPriority); }
            catch (Exception ex) {
                processingEnv.getMessager().printMessage(Diagnostic.Kind.ERROR,
                    "Hook registry generation failed: " + ex.getMessage(), container);
                ex.printStackTrace();
            }
        }
        try { generatePluginJson(); } catch (Exception ignored) {}
        return true;
    }

    private int extractClassPriority(TypeElement container) {
        for (AnnotationMirror am : container.getAnnotationMirrors()) {
            if (am.getAnnotationType().toString().endsWith(".Hook")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("priority".equals(kv.getKey().getSimpleName().toString())) {
                        try { return Integer.parseInt(kv.getValue().getValue().toString()); } catch (Exception e) { return 1000; }
                    }
                }
            }
        }
        return 1000;
    }

    private String extractTarget(Element e) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before") || t.endsWith(".After") || t.endsWith(".Replace")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("target".equals(kv.getKey().getSimpleName().toString())) return kv.getValue().getValue().toString();
                }
            }
        }
        return null;
    }

    private String extractId(Element e) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before") || t.endsWith(".After") || t.endsWith(".Replace")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("id".equals(kv.getKey().getSimpleName().toString())) {
                        String v = kv.getValue().getValue().toString();
                        if (v != null && !v.isEmpty()) return v;
                    }
                }
            }
        }
        return e.getSimpleName().toString();
    }

    private int extractPriority(Element e, int classPriority) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before") || t.endsWith(".After") || t.endsWith(".Replace")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("priority".equals(kv.getKey().getSimpleName().toString())) {
                        try { return Integer.parseInt(kv.getValue().getValue().toString()); } catch (Exception ex) { return classPriority; }
                    }
                }
                return classPriority;
            }
        }
        return classPriority;
    }

    private String extractPointRaw(Element e) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before") || t.endsWith(".After") || t.endsWith(".Replace")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("point".equals(kv.getKey().getSimpleName().toString())) {
                        String v = kv.getValue().getValue().toString();
                        // enum constant may be like "ENTRY" or "HookPoint.ENTRY"
                        int dot = v.lastIndexOf('.');
                        if (dot >= 0) v = v.substring(dot + 1);
                        return v;
                    }
                }
            }
        }
        return null;
    }

    private String extractEffectivePoint(Element e) {
        // If @At present, its value overrides
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            if (am.getAnnotationType().toString().endsWith(".At")) {
                for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                    if ("value".equals(kv.getKey().getSimpleName().toString())) {
                        String v = kv.getValue().getValue().toString();
                        int dot = v.lastIndexOf('.');
                        if (dot >= 0) v = v.substring(dot + 1);
                        return v;
                    }
                }
            }
        }
        String p = extractPointRaw(e);
        if (p != null) return p;
        // defaults: Before -> ENTRY, After -> RETURN, Replace -> ENTRY
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before")) return "ENTRY";
            if (t.endsWith(".After")) return "RETURN";
            if (t.endsWith(".Replace")) return "ENTRY";
        }
        return "ENTRY";
    }

    private long extractRva(Element e) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            if (!am.getAnnotationType().toString().endsWith(".At")) continue;
            for (Map.Entry<? extends ExecutableElement, ? extends AnnotationValue> kv : am.getElementValues().entrySet()) {
                if ("rva".equals(kv.getKey().getSimpleName().toString())) {
                    try { return Long.parseLong(kv.getValue().getValue().toString()); } catch (Exception ex) { return -1; }
                }
            }
        }
        return -1;
    }

    private boolean hasAnnotation(Element e, String simple) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            if (am.getAnnotationType().toString().endsWith("." + simple)) return true;
        }
        return false;
    }

    private boolean hasParamType(Element method, String fqn) {
        ExecutableElement ee = (ExecutableElement) method;
        for (VariableElement p : ee.getParameters()) {
            if (p.asType().toString().contains(fqn.substring(fqn.lastIndexOf('.')+1))) return true;
        }
        return false;
    }

    private int hookPointToInt(String point) {
        switch (point) {
            case "ENTRY": return 0;
            case "RETURN": return 1;
            case "INVOKE": return 2;
            case "OFFSET": return 3;
            case "VTABLE": return 4;
            case "IAT": return 5;
            case "GOT": return 6;
            default: return 0;
        }
    }

    private int hookKindToInt(Element e) {
        for (AnnotationMirror am : e.getAnnotationMirrors()) {
            String t = am.getAnnotationType().toString();
            if (t.endsWith(".Before")) return 0;
            if (t.endsWith(".After")) return 1;
            if (t.endsWith(".Replace")) return 2;
        }
        // At alone -> Raw (treat as Before at offset)
        if (hasAnnotation(e, "At")) return 0;
        return 0;
    }

    private void generateRegistry(TypeElement container, List<Element> methods, int classPriority) throws IOException {
        String pkg = processingEnv.getElementUtils().getPackageOf(container).getQualifiedName().toString();
        String cls = container.getSimpleName().toString();
        String regName = cls + "OrcaRegistry";
        String fqReg = pkg.isEmpty() ? regName : pkg + "." + regName;

        StringBuilder sb = new StringBuilder();
        if (!pkg.isEmpty()) sb.append("package ").append(pkg).append(";\n\n");
        sb.append("import org.orcaslicer.plugin.v1.*;\n");
        sb.append("final class ").append(regName).append(" {\n");
        sb.append("  static void registerAll() {\n");
        for (Element m : methods) {
            String target = extractTarget(m);
            if (target == null) target = "";
            String hookId = extractId(m);
            int priority = extractPriority(m, classPriority);
            String pointStr = extractEffectivePoint(m);
            int point = hookPointToInt(pointStr);
            int kind = hookKindToInt(m);
            long rva = extractRva(m);
            // For OFFSET, rva is offset absolute; for others, 0 means use symbol id
            long targetRvaParam = 0L;
            if ("OFFSET".equals(pointStr) && rva >= 0) {
                targetRvaParam = rva;
            } else if (rva >= 0 && pointStr.equals("OFFSET")) {
                targetRvaParam = rva;
            }
            String targetEsc = escape(target);
            String hookIdEsc = escape(hookId);
            String methodName = m.getSimpleName().toString();
            ExecutableElement ee = (ExecutableElement) m;
            boolean hasHookContext = false;
            boolean hasCpuContext = false;
            boolean hasNext = false;
            for (VariableElement p : ee.getParameters()) {
                String tn = p.asType().toString();
                if (tn.contains("HookContext")) hasHookContext = true;
                if (tn.contains("CpuContext")) hasCpuContext = true;
                if (tn.contains("Next")) hasNext = true;
            }
            sb.append("    // hook ").append(hookId).append(" -> ").append(target).append(" point=").append(pointStr).append(" kind=").append(kind).append(" prio=").append(priority).append("\n");
            sb.append("    {\n");
            sb.append("      final String _hook = \"").append(hookIdEsc).append("\";\n");
            sb.append("      final String _target = \"").append(targetEsc).append("\";\n");
            sb.append("      Object _cb = new Object() {\n");
            sb.append("        public void handle(long ctxPtr, long outPtr) {\n");
            if (hasNext) {
                sb.append("          Next _next = new Next(ctxPtr);\n");
            }
            if (hasHookContext || hasCpuContext || ee.getParameters().size() > 0) {
                sb.append("          CpuContext _raw = new CpuContext();\n");
                sb.append("          try {\n");
                sb.append("            _raw.rax = NativeBridge.readU64(ctxPtr + 8);\n");
                sb.append("            _raw.rbx = NativeBridge.readU64(ctxPtr + 16);\n");
                sb.append("            _raw.rcx = NativeBridge.readU64(ctxPtr + 24);\n");
                sb.append("            _raw.rdx = NativeBridge.readU64(ctxPtr + 32);\n");
                sb.append("            _raw.rsi = NativeBridge.readU64(ctxPtr + 40);\n");
                sb.append("            _raw.rdi = NativeBridge.readU64(ctxPtr + 48);\n");
                sb.append("            _raw.rbp = NativeBridge.readU64(ctxPtr + 56);\n");
                sb.append("            _raw.rsp = NativeBridge.readU64(ctxPtr + 64);\n");
                sb.append("            _raw.r8 = NativeBridge.readU64(ctxPtr + 72);\n");
                sb.append("            _raw.r9 = NativeBridge.readU64(ctxPtr + 80);\n");
                sb.append("            _raw.r10 = NativeBridge.readU64(ctxPtr + 88);\n");
                sb.append("            _raw.r11 = NativeBridge.readU64(ctxPtr + 96);\n");
                sb.append("            _raw.r12 = NativeBridge.readU64(ctxPtr + 104);\n");
                sb.append("            _raw.r13 = NativeBridge.readU64(ctxPtr + 112);\n");
                sb.append("            _raw.r14 = NativeBridge.readU64(ctxPtr + 120);\n");
                sb.append("            _raw.r15 = NativeBridge.readU64(ctxPtr + 128);\n");
                sb.append("            _raw.rip = NativeBridge.readU64(ctxPtr + 136);\n");
                sb.append("            _raw.rflags = NativeBridge.readU64(ctxPtr + 144);\n");
                sb.append("          } catch (Throwable _ignore) {}\n");
                sb.append("          HookTarget _tgt = new HookTarget(0, 0L, _target);\n");
                sb.append("          HookContext _ctx = new HookContext(_raw, _tgt);\n");
                String callArgs = "";
                if (hasNext && hasHookContext) {
                    callArgs = "_next, _ctx";
                } else if (hasNext && hasCpuContext) {
                    callArgs = "_next, _raw";
                } else if (hasNext) {
                    callArgs = "_next";
                } else if (hasHookContext) {
                    callArgs = "_ctx";
                } else if (hasCpuContext) {
                    callArgs = "_raw";
                } else if (ee.getParameters().size() == 0) {
                    callArgs = "";
                } else {
                    callArgs = "_ctx";
                }
                sb.append("          ").append(cls).append(".").append(methodName).append("(").append(callArgs).append(");\n");
            } else {
                sb.append("          ").append(cls).append(".").append(methodName).append("();\n");
            }
            sb.append("        }\n");
            sb.append("      };\n");
            String combined = hookIdEsc + "|" + targetEsc;
            sb.append("      int _st = NativeBridge.nativeInstallHook(\"").append(escape(combined)).append("\", ").append(targetRvaParam).append("L, ").append(point).append(", ").append(kind).append(", ").append(priority).append(", _cb);\n");
            sb.append("      if (_st != 0) NativeBridge.nativeLog(\"[orcareg] install \" + _hook + \" failed:\" + _st);\n");
            sb.append("    }\n");
        }
        sb.append("  }\n");
        sb.append("}\n");

        FileObject fo = processingEnv.getFiler().createSourceFile(fqReg, container);
        try (Writer w = fo.openWriter()) { w.write(sb.toString()); }
    }

    private void generatePluginJson() throws IOException {
    }

    private String escape(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }

    static class ManifestIndex {
        static class Symbol {
            String id, name;
            boolean typedAvailable;
            String typedReason;
            long rva;
            Set<Long> validOffsets = new HashSet<>();
        }
        Map<String, Symbol> byIdOrName = new HashMap<>();
        Symbol resolve(String target) {
            Symbol s = byIdOrName.get(target);
            if (s != null) return s;
            for (Symbol v : byIdOrName.values()) if (target.equals(v.name)) return v;
            return null;
        }
        boolean isValidOffset(Symbol s, long rva) {
            if (rva < 0) return false;
            if (s.validOffsets.isEmpty()) return true;
            if (s.validOffsets.contains(rva)) return true;
            if (s.validOffsets.contains(rva - s.rva)) return true;
            return false;
        }
        static ManifestIndex load(File f) throws IOException {
            ManifestIndex idx = new ManifestIndex();
            if (!f.exists()) return idx;
            try {
                String text;
                if (f.getName().endsWith(".gz")) {
                    try (java.util.zip.GZIPInputStream gz = new java.util.zip.GZIPInputStream(new FileInputStream(f));
                         ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                        byte[] buf = new byte[8192];
                        int n;
                        while ((n = gz.read(buf)) != -1) out.write(buf, 0, n);
                        text = out.toString("UTF-8");
                    }
                } else {
                    text = new String(java.nio.file.Files.readAllBytes(f.toPath()), "UTF-8");
                }
                int pos = 0;
                while ((pos = text.indexOf("\"id\"", pos)) != -1) {
                    int colon = text.indexOf(":", pos);
                    int q1 = text.indexOf("\"", colon);
                    int q2 = text.indexOf("\"", q1+1);
                    if (q1<0||q2<0) break;
                    String id = text.substring(q1+1, q2);
                    int rvaPos = text.indexOf("\"rva\"", q2);
                    if (rvaPos==-1 || rvaPos - q2 > 2000) { pos = q2+1; continue; }
                    int rc = text.indexOf(":", rvaPos);
                    int comma = text.indexOf(",", rc);
                    int brace = text.indexOf("}", rc);
                    int end = (comma>0 && comma<brace) ? comma : brace;
                    String rvaStr = text.substring(rc+1, end).trim();
                    long rva = 0;
                    try { rva = Long.parseLong(rvaStr); } catch (Exception e) { pos = end+1; continue; }
                    int typedPos = text.indexOf("\"typed_binding\"", q2);
                    boolean available = true;
                    String reason = "";
                    if (typedPos>0 && typedPos < rvaPos+5000) {
                        int avPos = text.indexOf("\"available\"", typedPos);
                        if (avPos>0 && avPos - typedPos < 500) {
                            int avColon = text.indexOf(":", avPos);
                            int avEnd = text.indexOf(",", avColon);
                            String av = text.substring(avColon+1, avEnd).trim();
                            available = av.startsWith("true");
                        }
                    }
                    Symbol s = new Symbol();
                    s.id = id; s.name = id; s.rva = rva; s.typedAvailable = available; s.typedReason = reason;
                    int instrPos = text.indexOf("\"instructions\"", q2);
                    if (instrPos>0 && instrPos - q2 < 8000) {
                        int arrStart = text.indexOf("[", instrPos);
                        int arrEnd = text.indexOf("]", arrStart);
                        if (arrStart>0 && arrEnd>0 && arrEnd-arrStart < 20000) {
                            String arr = text.substring(arrStart, arrEnd);
                            int o = 0;
                            while ((o = arr.indexOf("\"offset\"", o)) != -1) {
                                int cc = arr.indexOf(":", o);
                                int c2 = arr.indexOf(",", cc);
                                if (c2==-1) c2 = arr.indexOf("}", cc);
                                String offStr = arr.substring(cc+1, c2).trim();
                                try { long off = Long.parseLong(offStr); s.validOffsets.add(off); s.validOffsets.add(rva + off); } catch (Exception e) {}
                                o = c2+1;
                            }
                        }
                        pos = arrEnd+1;
                    } else {
                        pos = q2+1;
                    }
                    idx.byIdOrName.put(id, s);
                    if (idx.byIdOrName.size() > 400000) break;
                }
            } catch (Exception e) {
            }
            return idx;
        }
    }
}
