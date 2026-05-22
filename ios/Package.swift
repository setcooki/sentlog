// swift-tools-version: 5.9
// Phase 0 stub Swift Package manifest. Real targets (Sentlog public API,
// SentlogC interop shim, vendored XCFramework wrapping the C++ core) wire
// up in Phase 1.

import PackageDescription

let package = Package(
    name: "Sentlog",
    platforms: [
        .iOS(.v16),
    ],
    products: [
        .library(name: "Sentlog", targets: ["Sentlog"]),
    ],
    targets: [
        .target(
            name: "Sentlog",
            dependencies: ["SentlogC"],
            path: "Sources/Sentlog"
        ),
        .target(
            name: "SentlogC",
            path: "Sources/SentlogC",
            publicHeadersPath: "include"
        ),
        .testTarget(
            name: "SentlogTests",
            dependencies: ["Sentlog"],
            path: "Tests"
        ),
    ]
)
