// swift-tools-version: 5.9
//
// Vendored SwiftPM manifest for ggerganov/whisper.cpp v1.7.1.
//
// Why vendored:
//   The upstream Package.swift at v1.7.1 declares
//   `resources: [.process("ggml-metal.metal")]` at the package root,
//   but the actual file lives at `ggml/src/ggml-metal.metal`. The
//   mismatch makes Xcode 15 fail with "undeclared identifier
//   SWIFTPM_MODULE_BUNDLE" when ggml-metal.m tries to load the
//   Metal shader from the (non-existent) bundle. Upstream removed
//   Package.swift entirely in v1.7.5+ (switched to systemLibrary
//   mode), so there's no upstream tag to point at that works.
//
//   This manifest mirrors the upstream sources/headers list but fixes
//   the resource path. Same byte-for-byte v1.7.1 source as the
//   Android FetchContent pin in android/sentlog/src/main/cpp/
//   CMakeLists.txt, so both platforms run identical C++ / ggml code
//   per ADR 0009.
//
// Bumping the pin:
//   Re-run the equivalent of
//     curl -sL https://github.com/ggerganov/whisper.cpp/archive/refs/tags/<tag>.tar.gz \
//          | tar xz
//   and copy the same file set (see the sources/headers lists below).

import PackageDescription

let package = Package(
    name: "whisper",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        .library(name: "whisper", targets: ["whisper"]),
    ],
    targets: [
        .target(
            name: "whisper",
            path: ".",
            exclude: [
                "LICENSE",
                "Package.swift",
            ],
            sources: [
                "src/whisper.cpp",
                "ggml/src/ggml.c",
                "ggml/src/ggml-aarch64.c",
                "ggml/src/ggml-alloc.c",
                "ggml/src/ggml-backend.cpp",
                "ggml/src/ggml-quants.c",
                "ggml/src/ggml-metal.m",
            ],
            // The Metal shader lives next to ggml-metal.m. Declared
            // as a processed resource so SwiftPM bundles it and
            // defines SWIFTPM_MODULE_BUNDLE for ggml-metal.m's
            // shader-loading path.
            resources: [
                .process("ggml/src/ggml-metal.metal"),
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("ggml/include"),
                .headerSearchPath("ggml/src"),
                .headerSearchPath("src"),
                .define("GGML_USE_ACCELERATE"),
                .define("GGML_USE_METAL"),
                .define("NDEBUG"),
                // Upstream uses unsafeFlags for -O3 and -fno-objc-arc;
                // those settings are taken on by SwiftPM in release
                // mode by default (O for optimizations, MRC for the
                // .m file because we don't enable ARC project-wide).
            ],
            cxxSettings: [
                .headerSearchPath("ggml/include"),
                .headerSearchPath("ggml/src"),
                .headerSearchPath("src"),
                .define("GGML_USE_ACCELERATE"),
                .define("GGML_USE_METAL"),
                .define("NDEBUG"),
            ],
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Foundation"),
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
