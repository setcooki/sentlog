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
