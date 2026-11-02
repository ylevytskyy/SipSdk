#!/usr/bin/env python3
"""
Generate sample application icons across iOS, macOS, Android, and Windows from a master icon.
"""

import os
import sys
import json
import struct
import shutil
import tempfile
import subprocess
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
DEFAULT_MASTER = ROOT_DIR / "docs" / "app-design" / "icons" / "option-1-glass-telephony.jpg"

def run(cmd, **kwargs):
    subprocess.run(cmd, check=True, **kwargs)

def resize_image(src: Path, dst: Path, width: int, height: int):
    dst.parent.mkdir(parents=True, exist_ok=True)
    run(["sips", "-s", "format", "png", "-z", str(height), str(width), str(src), "--out", str(dst)], stdout=subprocess.DEVNULL)

def generate_ico(png_files_with_sizes, dst_ico: Path):
    """Generate multi-resolution Windows ICO file containing PNG images."""
    dst_ico.parent.mkdir(parents=True, exist_ok=True)
    images = []
    for size, path in png_files_with_sizes:
        with open(path, "rb") as f:
            images.append((size, size, f.read()))
            
    with open(dst_ico, "wb") as f:
        # Header: reserved (0), type (1), count (len(images))
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        offset = 6 + len(images) * 16
        for w, h, data in images:
            w_byte = 0 if w >= 256 else w
            h_byte = 0 if h >= 256 else h
            f.write(struct.pack("<BBBBHHII", w_byte, h_byte, 0, 0, 1, 32, len(data), offset))
            offset += len(data)
        for _, _, data in images:
            f.write(data)

def generate_ios_icons(master_path: Path):
    print("==> Generating iOS AppIcon asset catalog...")
    ios_iconset_dir = ROOT_DIR / "SipSdk" / "Assets.xcassets" / "AppIcon.appiconset"
    ios_iconset_dir.mkdir(parents=True, exist_ok=True)

    specifications = [
        # idiom, size, scale, pixel_size, filename
        ("iphone", "20x20", "2x", 40, "icon_20x20@2x.png"),
        ("iphone", "20x20", "3x", 60, "icon_20x20@3x.png"),
        ("iphone", "29x29", "2x", 58, "icon_29x29@2x.png"),
        ("iphone", "29x29", "3x", 87, "icon_29x29@3x.png"),
        ("iphone", "40x40", "2x", 80, "icon_40x40@2x.png"),
        ("iphone", "40x40", "3x", 120, "icon_40x40@3x.png"),
        ("iphone", "60x60", "2x", 120, "icon_60x60@2x.png"),
        ("iphone", "60x60", "3x", 180, "icon_60x60@3x.png"),
        ("ipad", "20x20", "1x", 20, "icon_ipad_20x20@1x.png"),
        ("ipad", "20x20", "2x", 40, "icon_ipad_20x20@2x.png"),
        ("ipad", "29x29", "1x", 29, "icon_ipad_29x29@1x.png"),
        ("ipad", "29x29", "2x", 58, "icon_ipad_29x29@2x.png"),
        ("ipad", "40x40", "1x", 40, "icon_ipad_40x40@1x.png"),
        ("ipad", "40x40", "2x", 80, "icon_ipad_40x40@2x.png"),
        ("ipad", "76x76", "1x", 76, "icon_ipad_76x76@1x.png"),
        ("ipad", "76x76", "2x", 152, "icon_ipad_76x76@2x.png"),
        ("ipad", "83.5x83.5", "2x", 167, "icon_ipad_83.5x83.5@2x.png"),
        ("ios-marketing", "1024x1024", "1x", 1024, "icon_1024x1024.png"),
    ]

    images_json = []
    for idiom, size, scale, pixel_size, filename in specifications:
        dst = ios_iconset_dir / filename
        resize_image(master_path, dst, pixel_size, pixel_size)
        images_json.append({
            "size": size,
            "idiom": idiom,
            "filename": filename,
            "scale": scale
        })

    contents = {
        "images": images_json,
        "info": {
            "version": 1,
            "author": "xcode"
        }
    }

    with open(ios_iconset_dir / "Contents.json", "w") as f:
        json.dump(contents, f, indent=2)
    print(f"    Saved {len(specifications)} iOS icon assets to {ios_iconset_dir}")

def generate_macos_icons(master_path: Path):
    print("==> Generating macOS AppIcon.icns...")
    macos_dir = ROOT_DIR / "platforms" / "macos" / "sample" / "SipSdkMac"
    icns_dst = macos_dir / "AppIcon.icns"

    with tempfile.TemporaryDirectory() as td:
        iconset_dir = Path(td) / "AppIcon.iconset"
        iconset_dir.mkdir(parents=True, exist_ok=True)
        
        mac_sizes = [
            ("icon_16x16.png", 16),
            ("icon_16x16@2x.png", 32),
            ("icon_32x32.png", 32),
            ("icon_32x32@2x.png", 64),
            ("icon_128x128.png", 128),
            ("icon_128x128@2x.png", 256),
            ("icon_256x256.png", 256),
            ("icon_256x256@2x.png", 512),
            ("icon_512x512.png", 512),
            ("icon_512x512@2x.png", 1024),
        ]
        
        for name, size in mac_sizes:
            resize_image(master_path, iconset_dir / name, size, size)
            
        run(["iconutil", "-c", "icns", str(iconset_dir), "-o", str(icns_dst)])
        print(f"    Generated macOS ICNS at {icns_dst}")

    # Update macOS Info.plist
    info_plist = macos_dir / "Info.plist"
    if info_plist.exists():
        content = info_plist.read_text()
        if "<key>CFBundleIconFile</key>\n\t<string></string>" in content:
            new_content = content.replace(
                "<key>CFBundleIconFile</key>\n\t<string></string>",
                "<key>CFBundleIconFile</key>\n\t<string>AppIcon</string>"
            )
            info_plist.write_text(new_content)
            print("    Updated macOS Info.plist CFBundleIconFile to 'AppIcon'")

def generate_android_icons(master_path: Path):
    print("==> Generating Android launcher mipmaps...")
    res_dir = ROOT_DIR / "platforms" / "android" / "sample" / "src" / "main" / "res"
    
    android_sizes = [
        ("mipmap-mdpi", 48),
        ("mipmap-hdpi", 72),
        ("mipmap-xhdpi", 96),
        ("mipmap-xxhdpi", 144),
        ("mipmap-xxxhdpi", 192),
    ]
    
    for folder, size in android_sizes:
        target_dir = res_dir / folder
        target_dir.mkdir(parents=True, exist_ok=True)
        resize_image(master_path, target_dir / "ic_launcher.png", size, size)
        resize_image(master_path, target_dir / "ic_launcher_round.png", size, size)
        
    print(f"    Saved Android mipmaps across mdpi, hdpi, xhdpi, xxhdpi, xxxhdpi")

    # Update AndroidManifest.xml if icon not present
    manifest_path = ROOT_DIR / "platforms" / "android" / "sample" / "src" / "main" / "AndroidManifest.xml"
    if manifest_path.exists():
        manifest_text = manifest_path.read_text()
        if 'android:icon=' not in manifest_text:
            manifest_text = manifest_text.replace(
                '<application',
                '<application\n        android:icon="@mipmap/ic_launcher"\n        android:roundIcon="@mipmap/ic_launcher_round"'
            )
            manifest_path.write_text(manifest_text)
            print("    Updated AndroidManifest.xml with android:icon & android:roundIcon")

def generate_windows_icons(master_path: Path):
    print("==> Generating Windows sample app.ico...")
    wpf_dir = ROOT_DIR / "platforms" / "windows" / "sample" / "SipSdk.WpfSample"
    ico_dst = wpf_dir / "app.ico"
    
    with tempfile.TemporaryDirectory() as td:
        sizes = [16, 32, 48, 64, 128, 256]
        items = []
        for s in sizes:
            p = Path(td) / f"icon_{s}.png"
            resize_image(master_path, p, s, s)
            items.append((s, p))
        generate_ico(items, ico_dst)
        print(f"    Generated Windows multi-resolution icon at {ico_dst}")

    # Update SipSdk.WpfSample.csproj
    csproj = wpf_dir / "SipSdk.WpfSample.csproj"
    if csproj.exists():
        text = csproj.read_text()
        if "<ApplicationIcon>" not in text:
            text = text.replace(
                "<RootNamespace>SipSdk.WpfSample</RootNamespace>",
                "<RootNamespace>SipSdk.WpfSample</RootNamespace>\n    <ApplicationIcon>app.ico</ApplicationIcon>"
            )
            # Add Resource ItemGroup if not present
            if 'Resource Include="app.ico"' not in text:
                text = text.replace(
                    "</Project>",
                    "  <ItemGroup>\n    <Resource Include=\"app.ico\" />\n  </ItemGroup>\n\n</Project>"
                )
            csproj.write_text(text)
            print("    Updated SipSdk.WpfSample.csproj with <ApplicationIcon> and Resource item")

    # Update MainWindow.xaml
    main_window = wpf_dir / "MainWindow.xaml"
    if main_window.exists():
        mw_text = main_window.read_text()
        if 'Icon="app.ico"' not in mw_text:
            mw_text = mw_text.replace(
                'Title="SipSdk WPF Video Phone"',
                'Title="SipSdk WPF Video Phone"\n        Icon="app.ico"'
            )
            main_window.write_text(mw_text)
            print("    Updated MainWindow.xaml with Icon=\"app.ico\"")

def main():
    master_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MASTER
    if not master_path.exists():
        print(f"Error: master icon not found at {master_path}", file=sys.stderr)
        sys.exit(1)
        
    print(f"Using master icon: {master_path}")
    
    # Save master high-res PNG in docs/app-design/icons/app-icon-1024.png
    master_png = ROOT_DIR / "docs" / "app-design" / "icons" / "app-icon-1024.png"
    resize_image(master_path, master_png, 1024, 1024)
    print(f"Saved master 1024x1024 PNG at {master_png}")
    
    generate_ios_icons(master_png)
    generate_macos_icons(master_png)
    generate_android_icons(master_png)
    generate_windows_icons(master_png)
    
    print("\nALL SAMPLE APP ICONS GENERATED SUCCESSFULLY.")

if __name__ == "__main__":
    main()
