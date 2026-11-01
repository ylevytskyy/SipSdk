// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SipSdk",
    platforms: [
        .iOS("12.1")
    ],
    products: [
        .library(name: "SipSdk", targets: ["SipSdk", "SipSdkResources"])
    ],
    targets: [
        .binaryTarget(
            name: "BaresipCore",
            url: "https://github.com/ylevytskyy/SipSdk/releases/download/baresip-core-v4.12.0/BaresipCore.xcframework.zip",
            checksum: "822a981895ad00c1fc9a3bfe3f82838be55e292277ce70e8755857f2d71204ad"
        ),
        .target(
            name: "SipSdkResources",
            path: "Sources/SipSdkResources",
            resources: [.copy("Resources/config.baresip-v4")]
        ),
        .target(
            name: "SipSdk",
            dependencies: ["BaresipCore"],
            path: "Sources/SipSdk",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("Foundation"),
                .linkedFramework("UIKit"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("SystemConfiguration"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CFNetwork"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("CallKit"),
                .linkedLibrary("z"),
                .linkedLibrary("m")
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
