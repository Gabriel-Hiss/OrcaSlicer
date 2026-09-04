import groovy.json.JsonSlurper
plugins { java }
java { sourceCompatibility = JavaVersion.VERSION_25; targetCompatibility = JavaVersion.VERSION_25 }
repositories { mavenCentral() }
val reportFile = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json")
val buildId: String = if (reportFile.exists()) {
    val parsed = JsonSlurper().parseText(reportFile.readText()) as Map<*, *>
    val build = parsed["build"] as Map<*, *>
    build["build_id"] as String
} else {
    // Fallback: pick latest SDK directory
    val sdkRoot = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk")
    val dirs = sdkRoot.listFiles()?.filter { it.isDirectory }?.sortedBy { it.name } ?: emptyList()
    val latest = dirs.lastOrNull()?.name ?: "windows-x86_64-a05740f0-f549-1fc1-4c4c-44205044422e-1-def7bb773d28"
    latest
}
val sdkDir = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/$buildId/jvm")
sourceSets {
    main {
        java {
            srcDirs("src/main/java", File(sdkDir, "src/main/java"))
        }
        resources {
            srcDirs("src/main/resources")
        }
    }
}
tasks.withType<JavaCompile> {
    options.release.set(25)
    // Pass manifest/buildId to HookProcessor for validation (if processor is on path)
    // Use SDK's plugin.json as manifest source (already has correct build_id)
    val manifestPath = sdkDir.resolve("META-INF/orca/plugin.json").absolutePath
    options.compilerArgs.addAll(listOf("-Aorca.manifest=$manifestPath", "-Aorca.buildId=$buildId"))
}
tasks.jar {
    archiveBaseName.set("orca-java-example")
    // Ensure META-INF/orca/plugin.json is included
    from("src/main/resources") { include("META-INF/orca/plugin.json") }
}
