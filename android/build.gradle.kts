plugins {
    id("com.android.application") version "8.13.2" apply false
}

// Keep build output under target/ like the rest of the repository.
val buildBase = providers.environmentVariable("GRADLE_BUILD_DIR")
    .orElse("${rootProject.projectDir}/../target/android-build")
allprojects {
    layout.buildDirectory.set(file("${buildBase.get()}/${project.name}"))
}
