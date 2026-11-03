// swift-tools-version: 5.9
//
// SwiftPM manifest for the Capacitor plugin. The Capacitor CLI adds it
// to the app's CapApp-SPM package; the package and product name must be
// "SentlogCapacitor" (the CLI derives it from the npm package name).
// Replace the branch pin with a version once releases are tagged.

import PackageDescription

let package = Package(
    name: "SentlogCapacitor",
    platforms: [
        .iOS(.v16),
    ],
    products: [
        .library(name: "SentlogCapacitor", targets: ["SentlogCapacitor"]),
    ],
    dependencies: [
        .package(url: "https://github.com/ionic-team/capacitor-swift-pm.git", from: "8.0.0"),
        .package(url: "https://github.com/setcooki/sentlog.git", branch: "main"),
    ],
    targets: [
        .target(
            name: "SentlogCapacitor",
            dependencies: [
                .product(name: "Capacitor", package: "capacitor-swift-pm"),
                .product(name: "Cordova", package: "capacitor-swift-pm"),
                .product(name: "Sentlog", package: "sentlog"),
            ],
            path: "ios/Plugin"
        ),
    ]
)
