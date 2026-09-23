Multi-platform Builder
=============

[![CodeQL](https://github.com/Next2D/builder/actions/workflows/github-code-scanning/codeql/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/github-code-scanning/codeql)
[![Lint](https://github.com/Next2D/builder/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/lint.yml)
[![release](https://img.shields.io/github/v/release/Next2D/builder)](https://github.com/Next2D/builder/releases)
[![license](https://img.shields.io/github/license/Next2D/builder)](https://github.com/Next2D/builder/blob/main/LICENSE)

[日本語]\
1つのプロジェクトから多種多様なプラットフォーム向けのアプリケーションを作成するマルチプラットフォームビルダー。  

* Steam：Steamに対応したWindows版、macOS版、Linux版のアプリケーションの書き出しが可能。
* デスクトップ：デスクトップ環境向けに、Windows、macOS、Linux向けのアプリケーションを出力できます。
* スマートフォン：iOSおよびAndroidに対応したスマートフォン用アプリケーションとしての書き出しもサポート。
* Web（HTML）：ブラウザ上で動作するHTML形式のコンテンツも簡単に生成可能。

Next2D Frameworkを使えば、1つのソースコードから多様なデバイスやOSに最適化されたアプリケーションを効率的に開発・展開することができます。

[English]\
Multi-platform builder to create applications for a wide variety of platforms from a single project.  

* Steam: Allows export of applications for Windows, macOS, and Linux versions that are compatible with Steam.
* Desktop: Export of applications for Windows, macOS, and Linux for desktop environments is supported.
* Smartphone: Export as smartphone applications compatible with iOS and Android is also supported.
* Web (HTML): Easily generate HTML-format content that runs in a browser.

With Next2D Framework, you can efficiently develop and deploy applications optimized for diverse devices and operating systems from a single source code.

## Supported

| platform        | detail                              |
|-----------------|-------------------------------------|
| Steam:Windows   | Export an exe file                  |
| Steam:macOS     | Export application bundle (.app)    |
| Steam:Linux     | Export executable + resources       |
| web             | Export minfy'd JS files             |
| iOS             | open Xcode, and Export ipa          |
| Android         | open Android Studio, and Export apk |
| Xbox            | GDK native title (V8 + Dawn/WebGPU) |

Xbox exports a GDK native title that runs the Next2D JavaScript on the V8 engine and
renders with Dawn (WebGPU → D3D12), packaged as a C++ executable (no Electron/WebView).
Building the GDK package requires Windows + Visual Studio 2022 + Microsoft GDK + a devkit.
On other platforms the builder scaffolds the host project and stages assets only.
See the generated `xbox/README.md` for full build steps.

```bat
npx @next2d/builder --platform xbox --env prd
```

The prebuilt V8 engine is downloaded automatically on the first Xbox build
(published from the `build-v8` workflow). To use your own build, pass
`--v8-root C:\path\to\v8`.

## Platform guides

Platform-specific configuration, dependencies and workflows are documented in these guides:

| Guide | Contents |
|---|---|
| [Electron / Steam](docs/steam.md) | Desktop exports, icons, architectures, signing, Steam depots, uploads and beta testing |
| [iOS / Android (Capacitor)](docs/capacitor.md) | Native configuration, SDKs, plugins, build commands, migration and Xcode setup |

### Scheduled introduction

| platform        | 
|-----------------|
| Nintendo Switch |

### build example.

```linux
npx @next2d/builder --platform web --env prd
```

### preview example.

```linux
npx @next2d/builder --preview --platform web --env prd
```

## License
This project is licensed under the [MIT License](https://opensource.org/licenses/MIT) - see the [LICENSE](LICENSE) file for details.

## Build dependencies

The builder uses the target project's installed Vite for configuration loading,
web builds and Xbox host-script minification. Run `npm install` in your game first;
Vite is not installed again as a builder runtime dependency. Vite 7 and 8 are supported.

The builder has no npm runtime dependencies. Node.js with npm/npx is required.
Terminal colors use Node.js `util.styleText`; Node typings and the builder's own Vite
installation are development dependencies.
