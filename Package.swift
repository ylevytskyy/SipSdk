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
            checksum: "2cfebbbf4c5e750ee46c17a344e0cd266d03c3890d0059e0f93f689742287715"
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
