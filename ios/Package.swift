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
        // (subsequent chunk) inference. Pinned to match the Android
        // onnxruntime-android version so both platforms run the same
        // model in the same runtime (see ADR 0009).
        .package(
            url: "https://github.com/microsoft/onnxruntime-swift-package-manager",
            from: "1.17.1"
        ),
    ],
    targets: [
        .target(
            name: "Sentlog",
            dependencies: [
                .product(name: "SentlogCore", package: "shared"),
                .product(name: "onnxruntime",
                         package: "onnxruntime-swift-package-manager"),
            ],
            path: "Sources/Sentlog",
            resources: [
                .process("PrivacyInfo.xcprivacy"),
                .copy("Resources/silero_vad.onnx"),
            ]
        ),
        .testTarget(
            name: "SentlogTests",
            dependencies: ["Sentlog"],
            path: "Tests/SentlogTests"
        ),
    ]
)
