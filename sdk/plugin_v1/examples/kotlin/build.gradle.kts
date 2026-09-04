import groovy.json.JsonSlurper
plugins { kotlin("jvm") version "2.4.0" }
repositories { mavenCentral() }
val reportFile = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/report/hook-sdkgen-report.json")
val buildId: String = if (reportFile.exists()) {
    val parsed = JsonSlurper().parseText(reportFile.readText()) as Map<*, *>
    val build = parsed["build"] as Map<*, *>
    build["build_id"] as String
} else {
    val sdkRoot = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk")
    val dirs = sdkRoot.listFiles()?.filter { it.isDirectory }?.sortedBy { it.name } ?: emptyList()
    dirs.lastOrNull()?.name ?: "windows-x86_64-a05740f0-f549-1fc1-4c4c-44205044422e-1-def7bb773d28"
}
val sdkDir = file("../../../../cmake-build-relwithdebinfo-visual-studio-llvm/generated/plugin-sdk/$buildId/jvm")
kotlin { jvmToolchain(25) }
sourceSets {
    main {
        java {
            // Kotlin plugin handles both java and kotlin sources; include SDK java
            srcDirs("src/main/kotlin", File(sdkDir, "src/main/java"), File(sdkDir, "src/main/kotlin"))
        }
        resources {
            srcDirs("src/main/resources")
        }
    }
}
dependencies {
    implementation(kotlin("stdlib"))
}
tasks.withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile> {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_25) }
}
tasks.withType<JavaCompile> { options.release.set(25) }
// Generate correct plugin.json at build time from SDK's build_id (overrides src/main/resources file if outdated)
tasks.named<Jar>("jar") {
    archiveBaseName.set("orca-kotlin-example")
    doFirst {
        val srcPluginJson = file("src/main/resources/META-INF/orca/plugin.json")
        if (srcPluginJson.exists()) {
            val txt = srcPluginJson.readText()
            if (!txt.contains(buildId)) {
                // Update src file for consistency (will be used in jar)
                val updated = txt.replace(Regex("windows-x86_64-[a-z0-9\\-]+"), buildId)
                srcPluginJson.writeText(updated)
            }
        }
    }
    from("src/main/resources") { include("META-INF/orca/plugin.json") }
}
