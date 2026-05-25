// swift-tools-version: 5.9
//
// SwiftPM manifest for the shared/ C++ core. Lives alongside
// CMakeLists.txt — the same source tree feeds two build systems
// without divergence.
//
// Consumed from ios/Package.swift via:
//
//   dependencies: [ .package(path: "../shared") ]
//
// and the iOS Swift target lists SentlogCore as a product dependency
// to import the C ABI from `<sentlog/sentlog.h>`.

import PackageDescription

let package = Package(
    name: "SentlogCore",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),     // for unit-test runs on the SwiftPM macOS toolchain
    ],
    products: [
        .library(name: "SentlogCore", targets: ["SentlogCore"]),
    ],
    targets: [
        .target(
            name: "SentlogCore",
            path: ".",
            exclude: [
                "CMakeLists.txt",
                "Package.swift",
                "tests",
                "build",
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
    ],
    cxxLanguageStandard: .cxx17
)
