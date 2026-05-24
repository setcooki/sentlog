// swift-tools-version: 5.9
//
// iOS / macOS SwiftPM manifest for Sentlog.
//
// Depends on ../shared (the C++ shared core) as a local SwiftPM
// package. The Sentlog Swift target imports SentlogCore and calls
// the C ABI declared in <sentlog/sentlog.h>.

import PackageDescription

let package = Package(
    name: "Sentlog",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        .library(name: "Sentlog", targets: ["Sentlog"]),
    ],
    dependencies: [
        .package(path: "../shared"),
        // ONNX Runtime — Silero VAD (this chunk) and AASIST-L liveness
        // (subsequent chunk) inference. Pinned to the 1.17.x line so
        // both platforms run the same model in the same runtime (see
        // ADR 0009). The SwiftPM repo only publishes 1.17.0; Android
        // is pinned to 1.17.1 on Maven (libs.versions.toml). Newer
        // ORT SwiftPM releases require macOS 14, which would push our
        // minimum past iOS 16's contemporary, so we hold at 1.17.x.
        .package(
            url: "https://github.com/microsoft/onnxruntime-swift-package-manager",
            exact: "1.17.0"
        ),
        // whisper.cpp — Phase 2 ASR. Pinned by revision (the v1.7.1
        // tag SHA) because the upstream Package.swift uses
        // `.unsafeFlags(...)` for ggml's -O3 / -fno-objc-arc tuning,
        // and SwiftPM forbids version-based pinning on packages with
        // unsafe flags. .revision() is the supported escape hatch.
        // Same tag SHA as the Android FetchContent pin in
        // android/sentlog/src/main/cpp/CMakeLists.txt so both
        // platforms run identical C++ / ggml code per ADR 0009.
        // CPU-only — Core ML acceleration on iOS is a follow-up.
        .package(
            url: "https://github.com/ggerganov/whisper.cpp",
            revision: "ebca09a3d1033417b0c630bbbe607b0f185b1488"  // v1.7.1
        ),
    ],
    targets: [
        .target(
            name: "Sentlog",
            dependencies: [
                .product(name: "SentlogCore", package: "shared"),
                .product(name: "onnxruntime",
                         package: "onnxruntime-swift-package-manager"),
                .product(name: "whisper", package: "whisper.cpp"),
            ],
            path: "Sources/Sentlog",
            resources: [
                .process("PrivacyInfo.xcprivacy"),
                .copy("Resources/silero_vad.onnx"),
                // AASIST-L is added here once tools/aasist-conversion/
                // convert-aasist-l.py has produced
                //     ios/Sources/Sentlog/Resources/aasist_l.onnx
                // Uncomment the line below at the same commit that
                // adds the binary. AasistLivenessRunner.swift already
                // handles the missing-file case (returns nil), so
                // leaving it commented while the model is absent is
                // safe — but the model bundle is inert on iOS until
                // both lines land together.
                // .copy("Resources/aasist_l.onnx"),
                // whisper.cpp model — same operator workflow as
                // AASIST-L. tools/whisper-model/download-tiny.sh
                // stages it; uncomment when the file is committed.
                // .copy("Resources/whisper-tiny.bin"),
            ]
        ),
        .testTarget(
            name: "SentlogTests",
            dependencies: ["Sentlog"],
            path: "Tests/SentlogTests"
        ),
    ]
)
