# iOS / Android（Capacitor）

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

## ゲーム側に残すもの

- `capacitor.config.json`：ゲーム固有設定。`ios.path` / `android.path` のカスタム出力先も使用できる。
- `ios/` / `android/`：ネイティブプロジェクト。既存のアイコン・署名設定・ネイティブコードを維持する。
- ゲーム固有のCapacitor/Cordovaプラグイン：ゲームの依存に置く。CLIは従来どおりゲームの依存とフックを参照する。

ゲームのJavaScriptで `@capacitor/core` を直接importする場合は、ビルド時の依存解決に必要なため、
`@capacitor/core` をゲームの直接依存としても宣言する。その場合はbuilderのCapacitorと
互換性のあるバージョンを指定する。今回のSlimeTenPuzzleにはそのimportはない。

Xcode、Android Studio、Android SDK、対応JDK、署名証明書などは引き続き実行環境に必要。
SDKのnpm依存を集約しても、端末実行・APK/AAB/IPAの生成に必要なネイティブツールは変わらない。

## iOSで `xcodebuild requires Xcode` が出る場合

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

## 既存プロジェクトの移行

1. この機能を含む公開済みbuilderを `npx` で使用する。
2. ゲームが直接importしていない `@capacitor/cli`・`@capacitor/core`・`@capacitor/ios`・`@capacitor/android` を直接依存から外す。
3. npmコマンド経由で `open:ios` / `open:android` を実行し、ネイティブ依存の参照先を再同期する。

隣接リポジトリで開発する場合はbuilderで `npm ci` または `npm install` を実行し、
ゲームでも `npm install` を実行する。builderのソース変更後はbuilderで `npm run build` を実行する。
npxキャッシュを削除した場合は、IDEが参照するSDKのパスを更新するため、
builderの `--open` / `--build` で再同期する。

[Capacitorの開発フロー](https://capacitorjs.com/docs/basics/workflow) /
[Capacitor設定](https://capacitorjs.com/docs/config)
