// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SipSdk",
    platforms: [
        .iOS("17.0")
    ],
    products: [
        .library(name: "SipSdk", targets: ["SipSdk"])
    ],
    targets: [
        .binaryTarget(
            name: "BaresipCore",
            url: "https://github.com/ylevytskyy/SipSdk/releases/download/baresip-core-v4.12.0/BaresipCore.xcframework.zip",
            checksum: "822a981895ad00c1fc9a3bfe3f82838be55e292277ce70e8755857f2d71204ad"
        ),
        .target(
            name: "SipCore",
            dependencies: ["BaresipCore"],
            path: "core",
            exclude: ["tests", "CMakeLists.txt"],
            sources: ["src"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("src"),
                .define("USE_UNIXSOCK", to: "1"),
                .define("SIP_CORE_BUILDING_LIB", to: "1")
            ]
        ),
        .target(
            name: "SipSdk",
            dependencies: ["BaresipCore", "SipCore"],
            path: "platforms/ios/Sources/SipSdk",
            resources: [
                .copy("Resources/config.baresip-v4"),
                .copy("Resources/PrivacyInfo.xcprivacy")
            ],
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
                .linkedFramework("PushKit"),
                .linkedLibrary("z"),
                .linkedLibrary("m")
            ]
        ),
        .testTarget(
            name: "SipSdkTests",
            dependencies: ["SipSdk"],
            path: "platforms/ios/Tests/SipSdkTests"
        )
    ],
    cxxLanguageStandard: .cxx17
)
