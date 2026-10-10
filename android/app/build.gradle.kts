plugins {
    id("com.android.application")
}

android {
    namespace = "com.github.costinm.espwifiloader"
    // Android 16 QPR2 (API 36.1) adds startLocalOnlyHotspotWithConfiguration
    // and the public SoftApConfiguration SSID setter: the only way an app can
    // host a network with a chosen name (DIRECT-xy-flash).
    compileSdk { version = release(36) { minorApiLevel = 1 } }

    defaultConfig {
        applicationId = "com.github.costinm.espwifiloader"
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"
    }
    buildTypes {
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
