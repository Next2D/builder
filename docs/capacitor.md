# iOS / Android（Capacitor）

[日本語](#日本語) | [English](#english)

## 日本語

### STEP1：対象OSのIDE・SDKを用意する

| 対象 | ユーザーが用意するもの |
|---|---|
| iOS | macOS、Xcode本体とiOS SDK。Xcodeを初回起動してセットアップを完了する。 |
| Android | Android Studio、Android SDK、使用するCapacitorに対応したJDK。 |
| 実機・配布ビルド | 対象端末と、その配布方法に必要な署名証明書・プロビジョニング設定またはAndroidの署名鍵。 |

使用するCapacitorのバージョンはbuilderの `src/tool-packages.ts` で固定している。
対応するネイティブ環境は[Capacitorの開発フロー](https://capacitorjs.com/docs/basics/workflow)から確認する。
Capacitorの `cli`・`core`・`ios`・`android` はbuilderが同じバージョンで取得するため、手動インストールは不要。

#### iOS：Xcodeの選択を確認する

```sh
xcode-select -p
xcodebuild -version
```

`/Library/Developer/CommandLineTools` が選択されていると、IPA生成時に `xcodebuild requires Xcode` で失敗する。
今回のシェルだけXcodeを指定する場合は、次を設定してからSTEP3へ進む:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

システム全体の既定を変える場合は、代わりに次を使う:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

Xcodeの配置先が異なる場合はパスを変更する。builderは呼び出し元の `DEVELOPER_DIR` を引き継ぐ。
[Appleのコマンドラインツール設定](https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings)

### STEP2：アプリ設定とプラグインを用意する

ゲームルートの `capacitor.config.json` にアプリ固有の値を設定する。

```json
{
  "appId": "app.example.game",
  "appName": "My Game",
  "webDir": "dist/ios/prd/"
}
```

| 項目 | 設定・管理方法 |
|---|---|
| `appId` / `appName` | 自分のアプリ識別子・表示名に変更する。 |
| `webDir` | builderが今回のWeb出力先に更新するため、OSごとの手動切り替えは不要。 |
| `ios.path` / `android.path` | 必要な場合だけネイティブプロジェクトのカスタム出力先を指定する。 |
| ゲーム固有のCapacitor/Cordovaプラグイン | ゲームの依存として管理する。ゲーム側のフックも引き続き使用する。 |
| `@capacitor/core` | ゲームのJavaScriptで直接importする場合だけ、builderのCapacitorと互換性のあるバージョンをゲームの直接依存に追加する。 |

その他の項目は[Capacitor設定](https://capacitorjs.com/docs/config)を参照。

### STEP3：ネイティブプロジェクトを開き、署名・アイコンを設定する

対象OSのコマンドを実行する。

```sh
npx @next2d/builder --platform ios --env prd --open
npx @next2d/builder --platform android --env prd --open
```

未作成の `ios/` / `android/` を生成し、Web資産とネイティブ依存を同期してIDEを開く。
Xcode / Android Studioで、アプリのアイコン・署名・配布方法に必要な設定を行う。
これらのネイティブプロジェクトはゲーム側で保持する。再実行時も既存の設定・ネイティブコードを維持する。

### STEP4：ビルドして対象環境で確認する

```sh
npx @next2d/builder --platform ios --env prd --build
npx @next2d/builder --platform android --env prd --build
```

実行前にWeb資産・ネイティブ依存を再同期する。対象端末やシミュレーター／エミュレーターで起動・入力・保存を確認する。
Capacitorの `cap run` を使う場合は `--build` を `--preview` に置き換える。
途中のコマンドが失敗した場合はbuilderも失敗する。

テンプレートのnpmスクリプト `open:ios` / `open:android` / `build:ios` / `build:android` も使用できる。
環境指定は `npm run build:ios -- --env prd` のように渡す。

### 補足：書き換えの検知

`sync` / `open` / `build` は、アプリに入れたスクリプト類（`.js` `.mjs` `.html` `.json` `.wasm`）と Capacitor の設定ファイルの SHA-256 を記録し、起動時に照合するコードをネイティブプロジェクトへ入れる。ビルドの後に書き換えられたファイルや、後から足されたスクリプトがあると、アプリは起動しない。画像や音声は対象外。

| OS | 入るもの | ゲームのリポジトリに残る変更 |
|---|---|---|
| Android | `app/src/main/java/app/next2d/integrity/` に照合のクラスと一覧（一覧は同じ場所の `.gitignore` で除外）。`AndroidManifest.xml` に `<provider>` を 1 つ | `AndroidManifest.xml` と照合のクラス（どちらも初回だけ） |
| iOS | `AppDelegate.swift` の末尾に照合の処理と、起動時の呼び出し 1 行。一覧は `App/App/public/next2d-integrity.txt` | `AppDelegate.swift`（初回だけ） |

- 一覧は `sync` のたびに、Capacitor がコピーした後のファイルから作り直す。IDE から Web 資材だけを入れ替えた（`cap copy` など）場合は一覧と合わなくなり、起動しなくなる。builder の `sync` / `build` をやり直すこと。
- `preview`（`cap run`）は照合しない一覧を書く。開発中に起動できなくならないようにするため。
- Android は、一覧をアプリのコードに持たせている。別の鍵で署名し直した APK でも、スクリプトを書き換えただけでは動かない。コードごと書き換える相手までは防げないので、配布経路での検証（Play Integrity など）と併用する。
- iOS は、アプリ全体が署名で封印されていて、書き換えたものは通常の端末では起動しない。この照合が効くのは、署名の検証が働かない端末でスクリプトだけを差し替えられた場合。一覧はバンドルの中にあるので、署名し直せる相手には効かない。

### 補足：既存プロジェクトの移行とSDKキャッシュ

1. ゲームから不要になった `@capacitor/cli`・`@capacitor/ios`・`@capacitor/android` の直接依存を外す。
   `@capacitor/core` とゲーム固有プラグインはSTEP2の条件に従う。
2. 設定と `ios/` / `android/` は保持し、STEP3でネイティブ依存の参照先を再同期する。

builderはゲームルートでCapacitor CLIを実行し、取得したSDKのパスを子プロセスの `NODE_PATH` に追加する。
通常・ネストされたnpm依存配置や `file:../builder` に対応し、ゲーム側の補助スクリプトやSDKシンボリックリンクは不要。
SDKキャッシュは書き出し後もGradle / CocoaPodsが参照するため保持する。
npxキャッシュを削除した場合は、STEP3またはSTEP4で参照先を再同期する。

## English

### STEP1: Prepare the platform IDE and SDK

| Target | What you need to prepare |
|---|---|
| iOS | macOS, the full Xcode application and the iOS SDK. Complete Xcode's first-launch setup. |
| Android | Android Studio, the Android SDK and a JDK compatible with the Capacitor version in use. |
| Device and distribution builds | Target devices and signing certificates/provisioning settings or an Android signing key, as required by the distribution method. |

The builder pins the Capacitor version in `src/tool-packages.ts`.
Check the corresponding native environment through the [Capacitor development workflow](https://capacitorjs.com/docs/basics/workflow).
The builder acquires matching versions of Capacitor's `cli`, `core`, `ios` and `android`; no manual installation is needed.

#### iOS: Check the selected Xcode

```sh
xcode-select -p
xcodebuild -version
```

If `/Library/Developer/CommandLineTools` is selected, IPA generation fails with `xcodebuild requires Xcode`.
To select Xcode for the current shell, set this before STEP3:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

Alternatively, to change the system-wide default:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
xcodebuild -version
```

Adjust the path if Xcode is installed elsewhere. The builder inherits the caller's `DEVELOPER_DIR`.
[Apple's command-line tools settings](https://developer.apple.com/documentation/xcode/configuring-command-line-tools-settings)

### STEP2: Prepare app configuration and plugins

Set game-specific values in `capacitor.config.json` at the game root.

```json
{
  "appId": "app.example.game",
  "appName": "My Game",
  "webDir": "dist/ios/prd/"
}
```

| Setting | Configuration / ownership |
|---|---|
| `appId` / `appName` | Replace with your app identifier and display name. |
| `webDir` | The builder updates this to the current web output directory; no manual switching between operating systems is needed. |
| `ios.path` / `android.path` | Set only when using custom native project locations. |
| Game-specific Capacitor/Cordova plugins | Keep them in the game's dependencies. Game hooks continue to work. |
| `@capacitor/core` | Add a compatible version as a direct game dependency only when the game's JavaScript imports it directly. |

See [Capacitor configuration](https://capacitorjs.com/docs/config) for other settings.

### STEP3: Open the native project and configure signing and icons

Run the command for your target OS:

```sh
npx @next2d/builder --platform ios --env prd --open
npx @next2d/builder --platform android --env prd --open
```

The builder creates missing `ios/` / `android/` projects, synchronizes web assets and native dependencies, then opens the IDE.
Configure app icons, signing and distribution-specific settings in Xcode / Android Studio.
Keep these native projects in the game. Subsequent runs preserve existing settings and native code.

### STEP4: Build and verify on the target environment

```sh
npx @next2d/builder --platform ios --env prd --build
npx @next2d/builder --platform android --env prd --build
```

Web assets and native dependencies are synchronized before building. Verify launch, input and saved data on target devices or simulators/emulators.
Replace `--build` with `--preview` to use Capacitor's `cap run`.
If any command fails, the builder fails as well.

Template npm scripts `open:ios` / `open:android` / `build:ios` / `build:android` are also available.
Pass the environment as in `npm run build:ios -- --env prd`.

### Reference: Detecting modified files

`sync` / `open` / `build` record the SHA-256 of the scripts packaged in the app (`.js` `.mjs` `.html` `.json` `.wasm`) and of the Capacitor config files, and add native code that checks them at launch. If a file was changed after the build, or a script was added, the app does not start. Images and audio are not checked.

| OS | What is added | Changes left in the game repository |
|---|---|---|
| Android | The check class and the list under `app/src/main/java/app/next2d/integrity/` (the list is excluded by a `.gitignore` in that folder). One `<provider>` in `AndroidManifest.xml` | `AndroidManifest.xml` and the check class (first time only) |
| iOS | The check and one call at launch, appended to `AppDelegate.swift`. The list is `App/App/public/next2d-integrity.txt` | `AppDelegate.swift` (first time only) |

- The list is rebuilt on every `sync` from the files Capacitor has just copied. If you replace only the web files from the IDE (`cap copy`, etc.) they no longer match the list and the app does not start; run the builder's `sync` / `build` again.
- `preview` (`cap run`) writes a list that checks nothing, so development runs are never blocked.
- Android keeps the list in the app's code. An APK re-signed with another key does not run if only its scripts were changed. It cannot stop someone who also patches the code, so combine it with verification on the distribution side (Play Integrity, etc.).
- On iOS the whole app is sealed by its signature and a modified app does not start on a normal device. This check matters only where signature validation is not enforced and just the scripts are swapped. The list is inside the bundle, so it does not stop someone who can re-sign the app.

### Reference: Existing project migration and SDK cache

1. Remove direct dependencies on `@capacitor/cli`, `@capacitor/ios` and `@capacitor/android` that are no longer needed by the game.
   Follow STEP2 for `@capacitor/core` and game-specific plugins.
2. Keep the configuration and `ios/` / `android/`, then use STEP3 to resynchronize native dependency paths.

The builder runs the Capacitor CLI from the game root and adds acquired SDK paths to the child process's `NODE_PATH`.
Regular and nested npm dependency layouts and `file:../builder` are supported; no game-side helper scripts or SDK symlinks are needed.
Keep the SDK cache after export because Gradle / CocoaPods reference it.
If you delete the npx cache, use STEP3 or STEP4 to resynchronize those paths.
