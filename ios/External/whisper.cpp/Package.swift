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
//   Same byte-for-byte v1.7.1 source as the Android FetchContent pin
//   in android/sentlog/src/main/cpp/CMakeLists.txt, so both
//   platforms run identical C++ / ggml code per ADR 0009.
//
// Two-target layout — required, not an aesthetic choice:
//   When a SwiftPM target declares `resources`, SwiftPM auto-
//   generates `resource_bundle_accessor.h` that `#import`s
//   <Foundation/Foundation.h>, and force-includes that file into
//   every source. That works for the .m (Obj-C) file but breaks
//   the .c / .cpp files because Foundation requires Obj-C mode.
//   Splitting the Obj-C ggml-metal.m + its shader resource into
//   its own target isolates the Foundation include.
//
//   The `whisper_metal` target carries the Metal backend + shader;
//   the `whisper` target carries the pure-C / C++ surface and
//   depends on `whisper_metal`. The `whisper` library product
//   exposes both via its dependency, so consumers `import whisper`
//   and get the full backend.

import PackageDescription

let package = Package(
    name: "whisper",
    platforms: [
        .iOS(.v16),
        .macOS(.v13),
    ],
    products: [
        .library(name: "whisper", targets: ["whisper", "whisper_metal"]),
    ],
    targets: [
        // Obj-C Metal backend — owns the shader resource. Isolated so
        // its SwiftPM-generated Foundation include doesn't break the
        // pure-C / C++ sources in the sibling target.
        .target(
            name: "whisper_metal",
            path: ".",
            exclude: [
                "LICENSE",
                "Package.swift",
                "VERSION.md",
                "include",
                "src",
                "ggml/src/ggml.c",
                "ggml/src/ggml-aarch64.c",
                "ggml/src/ggml-aarch64.h",
                "ggml/src/ggml-alloc.c",
                "ggml/src/ggml-backend.cpp",
                "ggml/src/ggml-backend-impl.h",
                "ggml/src/ggml-quants.c",
                "ggml/src/ggml-quants.h",
                "ggml/src/ggml-common.h",
                "ggml/src/ggml-cpu-impl.h",
                "ggml/src/ggml-impl.h",
            ],
            sources: [
                "ggml/src/ggml-metal.m",
            ],
            resources: [
                .process("ggml/src/ggml-metal.metal"),
            ],
            publicHeadersPath: "ggml/include",
            cSettings: [
                .headerSearchPath("ggml/src"),
                .define("GGML_USE_METAL"),
                .define("NDEBUG"),
                // ggml-metal.m manages CoreFoundation object
                // lifetimes manually with explicit retain/release —
                // it predates and is incompatible with ARC. SwiftPM
                // compiles .m files with ARC on by default; opt out
                // for this target. .unsafeFlags is fine here because
                // the consumer (ios/Package.swift) depends on us by
                // path — no version-based pinning is involved.
                .unsafeFlags(["-fno-objc-arc"]),
            ],
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Foundation"),
            ]
        ),
        // Pure-C / C++ ggml + whisper surface. Depends on the Metal
        // target so consumers `import whisper` get the full library.
        .target(
            name: "whisper",
            dependencies: ["whisper_metal"],
            path: ".",
            exclude: [
                "LICENSE",
                "Package.swift",
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
                .define("GGML_USE_METAL"),
                .define("NDEBUG"),
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
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
