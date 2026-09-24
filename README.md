# Multi-platform Builder

[![CodeQL](https://github.com/Next2D/builder/actions/workflows/github-code-scanning/codeql/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/github-code-scanning/codeql)
[![Lint](https://github.com/Next2D/builder/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/Next2D/builder/actions/workflows/lint.yml)
[![release](https://img.shields.io/github/v/release/Next2D/builder)](https://github.com/Next2D/builder/releases)
[![license](https://img.shields.io/github/license/Next2D/builder)](https://github.com/Next2D/builder/blob/main/LICENSE)

[日本語](#日本語) | [English](#english)

## 日本語

Next2D Frameworkの1つのプロジェクトから、複数のプラットフォーム向けにアプリケーションを書き出すビルダーです。
Steamおよびデスクトップ向けのWindows / macOS / Linux、スマートフォン向けのiOS / Android、Web（HTML）に対応しています。
Xbox対応は試作・開発段階です。

### 対応プラットフォーム

| プラットフォーム | 出力・対応状況 |
|---|---|
| Steam:Windows | 実行ファイル（.exe）と必要なリソース |
| Steam:macOS | アプリケーションバンドル（.app） |
| Steam:Linux | 実行ファイルと必要なリソース |
| Web | 最適化されたJavaScriptとWebアセット |
| iOS | Xcodeプロジェクトを開く、IPAを書き出す |
| Android | Android Studioプロジェクトを開く、APKを書き出す |
| Xbox | GDKネイティブホストの試作・開発段階 |

### プラットフォーム別ガイド

設定、依存ツール、書き出し・配布手順の詳細は、以下のドキュメントにまとめています。

| ガイド | 内容 |
|---|---|
| [Electron / Steam](docs/steam.md#日本語) | デスクトップ書き出し、アイコン、CPU、署名、Steam Depot、アップロード、ベータテスト |
| [iOS / Android（Capacitor）](docs/capacitor.md#日本語) | ネイティブ設定、SDK、プラグイン、ビルドコマンド、移行、Xcodeの設定 |
| [Xbox（試作）](docs/xbox.md#日本語) | 開発状況、ホスト生成、ビルド環境、検証範囲 |

### 今後の対応予定

| プラットフォーム |
|---|
| Nintendo Switch |

### ビルド例

```sh
npx @next2d/builder --platform web --env prd
```

### プレビュー例

```sh
npx @next2d/builder --preview --platform web --env prd
```

### ライセンス

このプロジェクトは[MIT License](https://opensource.org/licenses/MIT)で公開しています。詳細は[LICENSE](LICENSE)を参照してください。

## English

Build applications for multiple platforms from a single Next2D Framework project.
The builder supports Windows / macOS / Linux for Steam and desktop distribution, iOS / Android for mobile devices, and Web (HTML).
Xbox support is a prototype under development.

### Supported platforms

| Platform | Output / status |
|---|---|
| Steam:Windows | Executable (.exe) and required resources |
| Steam:macOS | Application bundle (.app) |
| Steam:Linux | Executable and required resources |
| Web | Optimized JavaScript and web assets |
| iOS | Open the Xcode project and export an IPA |
| Android | Open the Android Studio project and export an APK |
| Xbox | Prototype GDK native host; under development |

### Platform guides

These guides cover platform-specific configuration, dependencies, builds and distribution.

| Guide | Contents |
|---|---|
| [Electron / Steam](docs/steam.md#english) | Desktop exports, icons, architectures, signing, Steam depots, uploads and beta testing |
| [iOS / Android (Capacitor)](docs/capacitor.md#english) | Native configuration, SDKs, plugins, build commands, migration and Xcode setup |
| [Xbox (prototype)](docs/xbox.md#english) | Development status, host generation, build environment and validation scope |

### Planned support

| Platform |
|---|
| Nintendo Switch |

### Build example

```sh
npx @next2d/builder --platform web --env prd
```

### Preview example

```sh
npx @next2d/builder --preview --platform web --env prd
```

### License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT). See [LICENSE](LICENSE) for details.
