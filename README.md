Multi-platform Builder
=============

[![CodeQL](https://github.com/Next2D/builder/actions/workflows/codeql-analysis.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/codeql-analysis.yml)
[![Lint](https://github.com/Next2D/builder/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/lint.yml)
[![release](https://img.shields.io/github/v/release/Next2D/builder)](https://github.com/Next2D/builder/releases)
[![license](https://img.shields.io/github/license/Next2D/builder)](https://github.com/Next2D/builder/blob/main/LICENSE)

[日本語]\
1つのプロジェクトから多種多様なプラットフォーム向けのアプリケーションを作成するマルチプラットフォームビルダー。  

* Steam用のアプリ：Steamに対応したWindows版、macOS版、Linux版のアプリケーションの書き出しが可能。
* デスクトップアプリ：デスクトップ環境向けに、Windows、macOS、Linux向けのアプリケーションを出力できます。
* スマートフォンアプリ：iOSおよびAndroidに対応したスマートフォン用アプリケーションとしての書き出しもサポート。
* Web（HTML）コンテンツ：ブラウザ上で動作するHTML形式のコンテンツも簡単に生成可能。

Next2D Frameworkを使えば、1つのソースコードから多様なデバイスやOSに最適化されたアプリケーションを効率的に開発・展開することができます。

[English]\
Multi-platform builder to create applications for a wide variety of platforms from a single project.  

* Apps for Steam: Allows export of Steam-compatible applications for Windows, macOS, and Linux.
* Desktop apps: Export applications for Windows, macOS, and Linux for desktop environments.
* Smartphone applications: Export as smartphone applications for iOS and Android is also supported.
* Web (HTML) content: Content in HTML format that runs in a browser can also be easily generated.

With Next2D Framework, you can efficiently develop and deploy applications optimized for diverse devices and operating systems from a single source code.

## Supported

| platform        | detail                   |
|-----------------|--------------------------|
| Windows         | Export an exe file       |
| macOS           | Export dmg, app file     |
| Linux           | Export deb file          |
| Steam:Windows   | Export an exe file       |
| Steam:macOS     | Export dmg, app file     |
| Steam:Linux     | Export deb file          |
| web             | Export minfy'd JS files  |

### Scheduled introduction

| platform        | 
|-----------------|
| iOS             |
| Android         |
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