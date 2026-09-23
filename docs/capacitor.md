# iOS / Android（Capacitor）

[日本語](#日本語) | [English](#english)

## 日本語

Capacitorの `cli`・`core`・`ios`・`android` はbuilderの `src/tool-packages.ts` で同じバージョンに固定し、
iOS / Androidの操作時に `npx` でまとめて取得する。builderの `dependencies` には含めない。
ゲームは `npx @next2d/builder` で起動し、`capacitor.config.json` で
アプリ名・App ID・プラットフォーム固有設定を管理する。
builderやCapacitorをゲームの `package.json` に直接宣言する必要はない。

```sh
npm run open:ios -- --env prd
npm run open:android -- --env prd
npm run build:ios -- --env prd
npm run build:android -- --env prd
```

builderを直接使う場合:

```sh
npx @next2d/builder --platform ios --env prd --open
npx @next2d/builder --platform android --env prd --build
```

builderはnpmキャッシュに取得したCapacitor CLIをNodeで起動し、ゲームルートを作業ディレクトリにする。
ゲームからSDKを解決できない場合に備え、子プロセスの `NODE_PATH` に取得したSDKのパスを追加する。
通常のnpmインストール、依存がネストされた配置、隣接リポジトリの `file:../builder` に対応する。
ゲーム側にCapacitor用の依存宣言や補助スクリプト、SDKへのシンボリックリンクを追加する必要はない。
npm/npxを利用できるNode.js環境が必要。初回はネットワーク接続が必要で、以降はnpmキャッシュを再利用する。
取得先は一時ホストと異なり書き出し後も残り、Gradle / CocoaPodsから参照できる。

`webDir` を今回のWeb出力先へ更新してから、未作成のネイティブプロジェクトに `cap add` を実行する。
`--open` / `--build` は `cap sync` の成功後に `cap open` / `cap build` を実行する。
`--preview` は `cap run` を実行する。各コマンドの終了を待ち、失敗した場合はbuilderも失敗する。

### ゲーム側に残すもの

- `capacitor.config.json`：ゲーム固有設定。`ios.path` / `android.path` のカスタム出力先も使用できる。
- `ios/` / `android/`：ネイティブプロジェクト。既存のアイコン・署名設定・ネイティブコードを維持する。
- ゲーム固有のCapacitor/Cordovaプラグイン：ゲームの依存に置く。CLIは従来どおりゲームの依存とフックを参照する。

ゲームのJavaScriptで `@capacitor/core` を直接importする場合は、ビルド時の依存解決に必要なため、
`@capacitor/core` をゲームの直接依存としても宣言する。その場合はbuilderのCapacitorと
互換性のあるバージョンを指定する。今回のSlimeTenPuzzleにはそのimportはない。

Xcode、Android Studio、Android SDK、対応JDK、署名証明書などは引き続き実行環境に必要。
SDKのnpm依存を集約しても、端末実行・APK/AAB/IPAの生成に必要なネイティブツールは変わらない。

### iOSで `xcodebuild requires Xcode` が出る場合

`xcode-select -p` が `/Library/Developer/CommandLineTools` を返す場合、
Command Line Toolsが選択されている。iOSのアーカイブ・IPA生成にはXcode本体とiOS SDKが必要。
Xcodeをインストールし、初回起動時のセットアップを完了させる。

`/Applications/Xcode.app` を今回のビルドだけで使う場合は、ゲームのルートで実行する:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  npx @next2d/builder --platform ios --env prd --build
```

コマンドライン全体の既定をXcodeに切り替える場合:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

Xcodeの配置先が異なる場合はパスを変更する。builderは呼び出し元の `DEVELOPER_DIR` を引き継ぐ。
選択方法の詳細は[Appleのコマンドラインツール設定](https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings)を参照。

### 既存プロジェクトの移行

1. この機能を含む公開済みbuilderを `npx` で使用する。
2. ゲームが直接importしていない `@capacitor/cli`・`@capacitor/core`・`@capacitor/ios`・`@capacitor/android` を直接依存から外す。
3. npmコマンド経由で `open:ios` / `open:android` を実行し、ネイティブ依存の参照先を再同期する。

隣接リポジトリで開発する場合はbuilderで `npm ci` または `npm install` を実行し、
ゲームでも `npm install` を実行する。builderのソース変更後はbuilderで `npm run build` を実行する。
npxキャッシュを削除した場合は、IDEが参照するSDKのパスを更新するため、
builderの `--open` / `--build` で再同期する。

[Capacitorの開発フロー](https://capacitorjs.com/docs/basics/workflow) /
[Capacitor設定](https://capacitorjs.com/docs/config)

## English

The builder pins Capacitor's `cli`, `core`, `ios` and `android` packages to the same version in
`src/tool-packages.ts` and acquires them together through `npx` when an iOS / Android operation runs.
They are not included in the builder's `dependencies`.
Games invoke `npx @next2d/builder` and manage the app name, App ID and platform-specific settings in
`capacitor.config.json`. Neither the builder nor Capacitor needs to be declared directly in the game's `package.json`.

```sh
npm run open:ios -- --env prd
npm run open:android -- --env prd
npm run build:ios -- --env prd
npm run build:android -- --env prd
```

To invoke the builder directly:

```sh
npx @next2d/builder --platform ios --env prd --open
npx @next2d/builder --platform android --env prd --build
```

The builder launches the cached Capacitor CLI with Node, using the game root as the working directory.
It adds the acquired SDK paths to the child process's `NODE_PATH` so the game can resolve the SDKs when needed.
This supports regular npm installations, nested dependencies and a sibling repository linked through `file:../builder`.
The game does not need extra Capacitor dependency declarations, helper scripts or SDK symlinks.
Node.js with npm/npx is required. The first download needs network access; subsequent runs reuse npm's cache.
Unlike a temporary host, the acquired SDKs remain after export so Gradle / CocoaPods can keep referencing them.

The builder updates `webDir` to the current web output directory before running `cap add` for a native project that does not exist yet.
`--open` / `--build` run `cap open` / `cap build` after `cap sync` succeeds.
`--preview` runs `cap run`. The builder waits for each command and fails if a command fails.

### What stays in the game

- `capacitor.config.json`: Game-specific settings, including custom native output paths through `ios.path` / `android.path`.
- `ios/` / `android/`: Native projects. Existing icons, signing settings and native code are preserved.
- Game-specific Capacitor/Cordova plugins: Keep these in the game's dependencies. The CLI continues to discover the game's dependencies and hooks.

If the game's JavaScript imports `@capacitor/core` directly, declare `@capacitor/core` as a direct game dependency
so the web build can resolve it. Choose a version compatible with the builder's Capacitor version.
SlimeTenPuzzle does not currently have this import.

Xcode, Android Studio, the Android SDK, a compatible JDK and signing certificates are still required in the build environment.
Centralizing the SDK npm dependencies does not change the native tools needed for device execution or APK/AAB/IPA generation.

### If iOS reports `xcodebuild requires Xcode`

If `xcode-select -p` returns `/Library/Developer/CommandLineTools`, the standalone Command Line Tools are selected.
Creating an iOS archive or IPA requires the full Xcode application and the iOS SDK.
Install Xcode and complete its first-launch setup.

To use `/Applications/Xcode.app` for a single build, run this from the game root:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  npx @next2d/builder --platform ios --env prd --build
```

To select Xcode as the system-wide command-line default:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

Adjust the path if Xcode is installed elsewhere. The builder inherits the caller's `DEVELOPER_DIR`.
See [Apple's command-line tools settings](https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings) for details.

### Migrating an existing project

1. Use a published builder version that includes this feature through `npx`.
2. Remove direct dependencies on `@capacitor/cli`, `@capacitor/core`, `@capacitor/ios` and `@capacitor/android` that the game does not import directly.
3. Run the `open:ios` / `open:android` npm scripts to resynchronize native dependency paths.

When developing with a sibling builder repository, run `npm ci` or `npm install` in the builder,
then run `npm install` in the game. After changing builder source files, run `npm run build` in the builder.
If the npx cache is deleted, use the builder's `--open` / `--build` to resynchronize the SDK paths referenced by the IDE.

[Capacitor development workflow](https://capacitorjs.com/docs/basics/workflow) /
[Capacitor configuration](https://capacitorjs.com/docs/config)
