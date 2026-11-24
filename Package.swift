// swift-tools-version: 5.9
import Foundation
import PackageDescription

let localXCFramework = ".build/baresip-xcframework/BaresipCore.xcframework"
let useLocalXCFramework = FileManager.default.fileExists(atPath: localXCFramework)

let baresipBinaryTarget: Target = useLocalXCFramework
    ? .binaryTarget(
        name: "BaresipCore",
        path: localXCFramework
    )
    : .binaryTarget(
        name: "BaresipCore",
        url: "https://github.com/ylevytskyy/SipSdk/releases/download/baresip-core-v4.12.1/BaresipCore.xcframework.zip",
        checksum: "00697cf808629d21aedbe11c9ba2ac0e08f4d2ebab9bbbbb9a39c9969cf8924c"
    )

let package = Package(
    name: "SipSdk",
    platforms: [
        .iOS("17.0")
    ],
    products: [
        .library(name: "SipSdk", targets: ["SipSdk"])
    ],
    targets: [
        baresipBinaryTarget,
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
                .define("USE_TLS", to: "1"),
                .define("USE_DTLS", to: "1"),
                .define("USE_OPENSSL", to: "1"),
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
                .linkedFramework("Security"),
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
