// swift-tools-version: 5.9
//
// SwiftPM manifest for Sentlog, at the repo root so apps can add the
// repository URL as a package dependency.
//
// All targets are declared here, not in nested packages under shared/
// and ios/External/: SwiftPM rejects local-path dependencies inside a
// package that is itself fetched by URL. shared/ also builds through
// shared/CMakeLists.txt from the same sources.

import PackageDescription

let package = Package(
    name: "Sentlog",
    platforms: [
        .iOS(.v16),
        .macOS(.v14),
    ],
    products: [
        .library(name: "Sentlog", targets: ["Sentlog"]),
    ],
    dependencies: [
        // ONNX Runtime — Silero VAD (this chunk) and AASIST-L liveness
        // (subsequent chunk) inference. Pinned to the same version as
        // Android (libs.versions.toml) so both platforms run the same
        // model in the same runtime (see ADR 0009). The SwiftPM repo
        // lags Maven, so its newest release sets the shared version.
        // 1.24.x requires macOS 14; macOS is only our test host.
        // rn/SentlogRn.podspec pins the same version.
        .package(
            url: "https://github.com/microsoft/onnxruntime-swift-package-manager",
            exact: "1.24.2"
        ),
    ],
    targets: [
        .target(
            name: "Sentlog",
            dependencies: [
                "SentlogCore",
                "whisper",
                .product(name: "onnxruntime",
                         package: "onnxruntime-swift-package-manager"),
            ],
            path: "ios/Sources/Sentlog",
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
            path: "ios/Tests/SentlogTests"
        ),
        // The C++ core; imported for the C ABI in <sentlog/sentlog.h>.
        .target(
            name: "SentlogCore",
            path: "shared",
            exclude: [
                "CMakeLists.txt",
                "tests",
            ],
            sources: [
                "src/c_api.cpp",
                "src/internal/ring_buffer.cpp",
                "src/internal/state.cpp",
                "src/internal/config.cpp",
                "src/internal/pipeline.cpp",
                "src/internal/text_match.cpp",
                "src/internal/fingerprint.cpp",
                "src/internal/prosody.cpp",
                "src/internal/envelope.cpp",
                "src/internal/signing.cpp",
                "src/internal/ota.cpp",
                "src/internal/vad/energy_vad.cpp",
                "external/tweetnacl/tweetnacl.c",
                "external/tweetnacl/randombytes_stub.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("external"),
            ],
            cxxSettings: [
                .headerSearchPath("src"),
                .headerSearchPath("external"),
                .define("SENTLOG_BUILD_SWIFTPM"),
            ]
        ),
        // Vendored whisper.cpp v1.7.1 — why it's vendored and how to bump
        // it: ios/External/whisper.cpp/VERSION.md. CPU only, matching
        // WhisperRunner's use_gpu = false: the Metal backend isn't compiled
        // because it needs -fno-objc-arc, and SwiftPM rejects unsafeFlags in
        // a package that apps depend on by version.
        .target(
            name: "whisper",
            path: "ios/External/whisper.cpp",
            exclude: [
                "LICENSE",
                "VERSION.md",
                "ggml/src/ggml-metal.m",
                "ggml/src/ggml-metal.metal",
                "ggml/include/ggml-metal.h",
            ],
            sources: [
                "src/whisper.cpp",
                "ggml/src/ggml.c",
                "ggml/src/ggml-aarch64.c",
                "ggml/src/ggml-alloc.c",
                "ggml/src/ggml-backend.cpp",
                "ggml/src/ggml-quants.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("ggml/include"),
                .headerSearchPath("ggml/src"),
                .headerSearchPath("src"),
                .define("GGML_USE_ACCELERATE"),
                .define("NDEBUG"),
            ],
            cxxSettings: [
                .headerSearchPath("ggml/include"),
                .headerSearchPath("ggml/src"),
                .headerSearchPath("src"),
                .define("GGML_USE_ACCELERATE"),
                .define("NDEBUG"),
            ],
            linkerSettings: [
                .linkedFramework("Accelerate"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
