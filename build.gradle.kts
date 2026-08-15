// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    alias(libs.plugins.android.application) apply false
}

tasks.register("copyZygiskFiles") {
    description = "Copy Zygisk Files"
    dependsOn(":zygisk:assembleRelease")

    val moduleFolder = project.rootDir.resolve("module")
    val zygiskBuildDir = project.rootDir.resolve("zygisk/build")
    val classesJar = zygiskBuildDir.resolve("intermediates/dex/release/minifyReleaseWithR8/classes.dex")
    val zygiskSoDir = zygiskBuildDir.resolve("intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib")
    val destDex = moduleFolder.resolve("classes.dex")
    val destZygisk = moduleFolder.resolve("zygisk")

    inputs.file(classesJar)
    inputs.dir(zygiskSoDir)
    outputs.file(destDex)
    outputs.dir(destZygisk)

    doLast {
        classesJar.copyTo(destDex, overwrite = true)
        moduleFolder.resolve("inject").deleteRecursively()
        destZygisk.mkdirs()
        zygiskSoDir.walk()
            .filter { it.isFile && it.name == "libzygisk.so" }
            .forEach { soFile ->
                val abiFolder = soFile.parentFile.name
                soFile.copyTo(destZygisk.resolve("$abiFolder.so"), overwrite = true)
            }
    }
}

tasks.register<Zip>("zip") {
    description = "Zip Module"
    dependsOn("copyZygiskFiles")

    archiveFileName.set("PlayIntegrityFix.zip")
    destinationDirectory.set(project.rootDir.resolve("out"))

    from(project.rootDir.resolve("module"))
}
