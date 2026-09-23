# Electron / Steam 書き出し / Export

[日本語](#日本語) | [English](#english)

## 日本語

ローカル書き出しだけなら、Steamworks・SteamCMD・配布用の署名認証は不要。

### STEP1：対象OSの実行環境とアイコンを用意する

- macOSのUniversalアプリと配布用の署名・公証はmacOS上で行う。
- 配布する各OSで起動・入力・保存を確認できる実行環境を用意する。
- ゲームの `src/assets/icons/` にWindows用ICO、macOS用ICNS、Linux用PNGを用意する。
  テンプレートの仮アイコンは配布前に差し替える。設定の省略時の扱いはSTEP3の表を参照。

#### 対応OS・CPU

| OS | 既定値 | `architectures` / `--arch` の対応値 |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

32bit版Windows（`ia32`）には対応しない。`win32` はElectron内部のWindowsのOS名で、
32bitを意味しない。例えば `My Game-win32-x64/` は64bit版Windows向けの成果物。
`--platform` には `windows` または `steam:windows` を指定する。

macOSの `universal` はx64とarm64を含む1つの `.app`。

### STEP2：SteamworksのApp・Depot・テスト用ブランチを用意する（Steam配布時）

1. 対象アプリのSteamworks管理画面で、発行済みのApp IDを確認する。
2. SteamPipe > DepotsでDepotを作成し、対象OSを設定する。言語共通ならAll languagesを選び、
   開発用・販売用Packageにも対象Depotを含める。**Depot IDはApp IDから推測せず、実際の値を使う。**
3. SteamPipe > Buildsで `internal` ブランチを作成し、**パスワードを設定して関係者だけに共有する。**
   名前だけでは非公開にならず、builderはブランチ作成・パスワード設定や確認を行わない。
4. テスターにゲームと対象Depotの利用権を用意する。未発売ゲームの外部テスターには
   Release State Override（beta）キー等を使う。ブランチのパスワードだけでは利用権は付与されない。
   パスワードを第三者に共有しないよう運用し、JSONやリポジトリにも保存しない。

OS別Depotと共有Depotのどちらも使用できる。共有するOSには同じDepot IDを設定し、
Steamworks側の対象OSをAll OSesなど共有構成に合わせる。
共有Depotは全OSのファイルを配布するため容量が増える。ValveはOS固有ファイルには別Depotを推奨する。

[Depotの設定](https://partner.steamgames.com/doc/store/application/depots) /
[betaブランチ](https://partner.steamgames.com/doc/store/application/branches) /
[Steamでのテスト](https://partner.steamgames.com/doc/store/testing) /
[Release State Overrideキー](https://partner.steamgames.com/doc/features/keys)

### STEP3：electron.config.jsonを設定する

プロジェクトルートの `electron.config.json` でゲーム固有設定を管理する。
JavaScript / TypeScriptの両テンプレートに含まれるので、初期値を自分のゲームの値に変更する。
`appId` は macOS bundle ID・保存先識別子、`steam.appId` は Valve の数値 App ID。
これらは別のID。バージョンはゲームルートの `package.json` を使う。

```json
{
  "appId": "app.example.game",
  "appName": "My Game",
  "executableName": "my-game",
  "companyName": "Example Company",
  "icons": {
    "windows": "src/assets/icons/icon.ico",
    "macos": "src/assets/icons/icon.icns",
    "linux": "src/assets/icons/icon.png"
  },
  "architectures": { "windows": "x64", "macos": "universal", "linux": "x64" },
  "window": { "width": 1280, "height": 720, "fullscreen": false },
  "steam": {
    "appId": null,
    "branch": "internal",
    "depots": { "windows": null, "macos": null, "linux": null }
  },
  "macos": { "sign": false, "notarize": false }
}
```

| 項目 | 用途・仕様 | 省略時・注意点 |
|---|---|---|
| `companyName` | Windows実行ファイルの会社名。 | 省略時は `package.json` の `author.name`、なければゲーム名。 |
| `appName` / `executableName` | `appName` はウィンドウ・アプリケーション名。`executableName` は実行ファイル名。 | 表示名と実行ファイル名を個別に設定できる。 |
| `description` | 任意の説明文。生成するElectronホストの `package.json` とWindowsのファイル説明に反映する。 | JSONの値を優先。未指定または `null` ならルートの `package.json` の `description` を使う。両方未指定なら空文字。明示的な `""` も空文字として扱う。 |
| `icons` | アイコン画像のパス。プロジェクトルートからの相対パス、または絶対パスを指定する。WindowsはICO、macOSはICNS、LinuxはPNG。 | 各キーは省略可。省略したOSにはbuilder同梱の仮のNext2Dアイコンを使う。`"icons": {}` なら全OSで仮アイコンを使える。指定したファイルがなければ失敗する。 |
| `architectures` | OSごとのCPU設定。対応値はSTEP1の「対応OS・CPU」を参照。 | 省略時はWindows/Linuxが `x64`、macOSが `universal`。`--arch` が優先する。 |
| `steam.appId` | Valveの数値App ID。未発行なら `null` を指定する。 | 未発行でもローカル書き出しは可能。SteamPipe用VDFは生成しない。 |
| `steam.branch` | `--steam-upload` の反映先betaブランチ。 | 省略時は `internal`。`--steam-branch` が優先する。ブランチの作成・パスワード設定はSteamworks側で行う。 |
| `steam.depots` | Steamworksで作成した実際のDepot ID。複数OSに同じIDを指定すると、同一Depotへまとめて配布する。 | App IDから推測しない。`null` / 未指定ならアプリと起動情報だけを生成し、アップロード用VDFは生成しない。 |
| `macos` | macOSアプリの署名・公証設定。 | ローカル検証用は `sign` / `notarize` ともに `false`。本番配布では両方を有効化する。 |

### STEP4：署名・アップロードの認証を用意する（配布時）

#### macOSの署名・公証

macOSのキーチェーンにDeveloper ID Application証明書を登録し、
`xcrun notarytool store-credentials` で公証用プロファイルを保存する。認証情報はJSONへ書かない。

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
```

配布用は `macos.sign` / `macos.notarize` を両方 `true` にする。
またはSTEP5のmacOSコマンドに `NEXT2D_STEAM_RELEASE=1` を付けると、JSONのfalse指定に関わらず両方を必須にできる。
認証情報不足・署名失敗・公証失敗はビルド失敗となる。

Steam用のターゲットは `darwin`。Valveは新規macOSアプリに64bitとAppleの公証を要求する。
テンプレートのentitlementsはJITとSteam Overlay用のlibrary validation / DYLD設定を含み、
`com.apple.security.app-sandbox` は付けない（Chromiumのrenderer sandboxとは別）。
[Valveのプラットフォーム要件](https://partner.steamgames.com/doc/store/application/platforms) /
[Electron Packagerの署名・公証設定](https://electron.github.io/packager/main/interfaces/Options.html)

#### SteamCMDとビルドアカウント

[Steamworks SDKのContentBuilder](https://partner.steamgames.com/doc/sdk/uploading)にある実行OS用のSteamCMDを用意する。
Windowsは `steamcmd.exe`、macOS / Linuxは `steamcmd.sh` を使用できる。
PATH上の `steamcmd`（Windowsは `steamcmd.exe`）を使うか、環境変数 `STEAMCMD` に実行ファイルのパスを指定する。
引数やシェルコマンドを含めず、パスだけを指定する。`.sh` は実行権限が必要。
SteamCMD自体の実行に必要なOSライブラリはSteamCMDの手順に従って用意する。

アップロード用アカウントには対象アプリの `Edit App Metadata` と `Publish App Changes To Steam` の権限を付与する。
macOS / Linuxの設定例（配置先とアカウント名を置き換える）:

```sh
export STEAMCMD="/absolute/path/to/steamcmd.sh"
export STEAM_USERNAME="your_build_account"
"$STEAMCMD" +login "$STEAM_USERNAME" +quit
```

初回ログインではSteamCMDにパスワードとSteam Guardコードを対話入力する。
その後builderは同じSteamCMDの保存済みログインを使い、パスワードをコマンド引数へ渡さない。
認証が失効した場合は同じSteamCMDで再ログインする。builderからの実行は対話入力を待たず失敗させる。
CIではSteamCMDの `config/config.vdf` をSecretとして復元・管理し、ビルド成果物に含めない。
Windows PowerShellでは `$env:STEAMCMD` と `$env:STEAM_USERNAME` を設定する。

### STEP5：各OSを書き出し、成果物を揃える

ゲームルートで必要なOSのコマンドを実行する。書き出しはアップロード・公開を行わない。

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

CPUを変える場合は `--arch arm64` などを追加する。
テンプレートの `npm run build:steam:windows` / `build:steam:macos` / `build:steam:linux` も同じ処理を行い、
`--env prd` を指定済み。npm経由の引数は `-- --arch arm64` や `-- --env dev` で渡す。

既定の出力先は `dist/steam/<OS>/build/<env>/`。

| OS | 配布するフォルダ全体 | OS別DepotのExecutable | 共有DepotのExecutable |
|---|---|---|---|
| Windows | `My Game-win32-x64/` | `my-game.exe` | `windows/my-game.exe` |
| macOS | `My Game-darwin-universal/` | `My Game.app` | `macos/My Game.app` |
| Linux | `My Game-linux-x64/` | `my-game` | `linux/my-game` |

実際の起動パス・CPU・ContentRootは `*-steampipe/launch.json` を参照する。
実行ファイルだけでなく、Electronのライブラリ・locales・ライセンス・resourcesを含むフォルダ全体が配布対象。
Steam用のDMG/DEB/MSIは不要で、書き出し後に追加のアプリビルドは行わない。

別マシンやCIの成果物は、`steam-package.json` と `*-steampipe/` を含む各OSの `build/<env>/` を
同じ `dist/steam/<OS>/build/<env>/` 構成に集める。macOSのシンボリックリンクとLinuxの実行権限を保つため、
CI artifactは `tar.gz` にして転送する。カスタム出力先はSTEP6の `--steam-root` で指定する。

設定・バージョンを変えたら対象OSを再書き出しする。同じバージョン番号でも全OSを意図したリビジョンで揃える。
同じOSのx64とarm64を順に書き出すと、最後に成功したCPUの成果物を使う。
同じOSの複数CPUを1つの共有Depotへ同時に統合する処理は行わない。

配布用macOSアプリは署名・公証・staple完了を確認する（パスは成果物に合わせる）:

```sh
codesign --verify --deep --strict 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
```

### STEP6：起動設定を反映し、アップロード・テストする

SteamworksのInstallation > General InstallationにOS別Launch Optionを追加する。
ExecutableはSTEP5の `launch.json` に合わせ、Arguments/Working Directoryは空欄にする。
x64版は64bit条件を指定する。対応OS・最低OS要件は同梱Electronと実機検証に合わせ、Steamworksで変更をPublishする。

ゲームルートで次を実行する。共有Depot・OS別Depotをまとめて1回のAppBuildでアップロードする。
Web/Electronの再ビルドや事前の `--steam-manifest` 実行は不要。

```sh
# ローカル検証のみ。SteamCMD・認証・Steamへの接続は不要
npx @next2d/builder --steam-upload --env prd --dry-run

# 検証後、設定したブランチへアップロード
npx @next2d/builder --steam-upload --env prd
```

| 引数・設定 | 用途 |
|---|---|
| `--steam-branch internal` | `steam.branch` の反映先を上書き。`SetLive` で反映を要求する。`default` は指定不可で、一般公開用ビルドの切り替えはSteamworksで行う。 |
| `--steam-root dist/steam` | 成果物の収集先。既定は `dist/steam`。ゲームルートからの相対パスまたは絶対パス。アップロード時はVite設定を読まないため、カスタム出力先では明示する。 |
| `--dry-run` | パッケージと設定の整合性を検証する。Steam側の権限・ブランチ・パスワードの確認は行わない。 |
| 併用しない引数 | `--build` / `--preview` / `--open` / `--steam-manifest` / `--arch`。`--platform` も不要。 |

対応するテンプレートのnpmスクリプトは `upload:steam:check` / `upload:steam`。

builderは `steam.depots` に設定された全OSの `steam-package.json` と実行ファイルを検証し、
App ID・Depot ID・バージョン・ゲーム名・CPUに不足や不整合があればSteamCMD起動前に停止する。
VDFは検証結果から再生成する。通常の書き出し用VDFは変更しない。

| 保存先（`--steam-root` 配下） | 内容 |
|---|---|
| `uploads/<env>/<branch>-<識別子>/` | VDF、`upload-plan.json`。dry-runは `Preview=1`、`SetLive`なし。アップロード成功時はBuildIDを `upload-result.json` に記録。 |
| `uploads/<env>/cache/<branch>/` | SteamPipeのログ・差分キャッシュ。 |

SteamworksのBuilds画面で、記録されたBuildIDが対象ブランチの現在のビルドになっていることを確認する。
アカウントやアプリの状態によってSteam側の追加確認が必要になる場合がある。
テスターはSteamクライアントのプロパティ > ゲームバージョンとベータでパスワードを入力し、`internal` を選ぶ。
各OSでインストール・起動を確認し、macOS UniversalはIntel / Apple Siliconの両方でテストする。

### 参照：VDFの生成と手動アップロード

builderのSTEP6を使う場合、この節の操作は不要。SteamCMDを直接操作する場合に使う。

| 構成 | 通常の書き出し時のVDF生成先・条件 |
|---|---|
| OS別Depot | 各OSの `*-steampipe/`。 |
| 共有Depot | 全対象OSが揃うと `dist/steam/shared/<env>/depot-<ID>/`。不足OSは `launch.json` に記録し、アップロード用VDF・OS単独VDFは生成しない。 |

一部OSだけDepot IDを共有する構成にも対応する。`FileMapping` でOS別サブディレクトリへ配置し、アセットは再コピーしない。
別マシンの成果物を集めた後に共有DepotのVDFだけを再生成する場合:

```sh
npx @next2d/builder --platform steam:macos --env prd --steam-manifest
```

npmの別名は `build:steam:manifest`（別環境なら `-- --env dev`）。このコマンドはどのOSでも実行でき、
`--platform` はVite設定を読むための指定で、設定内のすべての共有Depotを統合する。
`build.outDir` と `--env` から最新の成功した書き出しを選び、App ID・Depot ID・ゲーム名・実行ファイル名・バージョンを検証する。
不足パッケージは終了コード1となり、古い統合VDFは再生成時に無効化する。
`--arch` / `--preview` / `--build` / `--open` とは併用できない。

`app_preview.vdf` はファイルマッピング検証、`app_build.vdf` はアップロード、
`depot_build.vdf` はフォルダ内容の再帰マッピング用。`steam_appid.txt` は配布対象から除外する。
ContentRootはVDFからの相対パスなので、単独Depotはアプリと `*-steampipe/` をセットで、共有Depotは `dist/steam/` 全体の構成を保って移動する。

```text
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_preview.vdf" +quit
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_build.vdf" +quit
```

通常の書き出し・`--steam-manifest` のVDFには `SetLive` がないため、アップロード後にSteamworksで対象ブランチへ設定する。
OS別VDFは1つのDepotを更新するので、最終BuildのDepot manifestが全OSで意図した版になっているか確認する。
[SteamPipe公式手順・VDF仕様](https://partner.steamgames.com/doc/sdk/uploading)

### 参照：テンプレート・ホスト・移行

`create-next2d-app` はプロジェクト名から `appId`、`appName`、`executableName`、
`companyName` を設定する。例えば `my-game` なら `appId` は `app.example.my-game`、
他の3項目は `my-game` になる。配布前にbundle IDと会社名を自分の値へ変更する。
CPU設定が省略されている場合はOSごとの既定値を補い、テンプレートに指定済みの値は保持する。
アイコンパスやSteam IDなどの設定も保持する。

Electron用コードはbuilderが管理する。OSの一時ディレクトリにホスト・runtime設定・Web資産を配置し、失敗時を含め書き出し後に削除する。
ゲーム側へ `electron/`、Electron用 `node_modules` / lockfileは作らない。
Electronは `templates/electron/package.json`、Packagerは `src/tool-packages.ts` の固定バージョンを取得・キャッシュする。
PackagerはElectron書き出し時のみ取得し、OS別パッケージ・アイコン・署名・公証を処理する。
ホストにはnpm依存やネイティブアドオンがないため、ABIに合わせた再ビルドは不要。
プレビューは実行ホストのOS/CPUで書き出した `dist/<platform>/build/<env>/` のアプリを起動する。

旧 `electron/` のホストコード・`config.forge`・独自npm依存は参照しない。
移行時はアイコンを共通アセットへ移し、JSONのパスを更新してから旧ディレクトリを削除する。
独自main/preloadやネイティブアドオンがある場合は、先にbuilder側への対応が必要。

テンプレートのホストは絶対パスと固定origin `next2d://game` で資産を読む。
起動時の作業ディレクトリに依存せず、ローカルfetch・Worker・localStorageが使える。
F11/Alt+Enterで全画面、Escapeで解除。閉じるとmacOSでもプロセスを終了する。
Node integration無効・context isolation/sandbox有効、外部ページ遷移と新規ウィンドウは禁止。
Next2D用CSPを付与する。外部APIを使うゲームでは接続先を明示的に追加すること。
[Electron security](https://www.electronjs.org/docs/latest/tutorial/security) /
[protocol](https://www.electronjs.org/docs/latest/api/protocol)

保存先はElectronのappData配下の `appId` ディレクトリ。
表示名を変えても保存先は変わらない。旧file-originの開発版セーブは自動移行しない。
Steam Cloudは未統合。Chromiumプロファイル全体をCloud対象にせず、導入時は
ゲーム用のセーブファイルとアカウント単位の保存方式を別途設計する。
[Steam Cloud](https://partner.steamgames.com/doc/features/cloud)

### リリース前の確認範囲

STEP6のインストール・起動確認に加えて、以下を検証する。

- 終了動作、オフライン起動、更新後の保存データ。
- マウス/キーボード/コントローラ、解像度・全画面切替、音声、スリープ復帰。
- Shift+Tab Overlay。Electronは複数プロセスなので、OS/GPUごとに実動作を確認する。
  この書き出しはSteamworks SDK・実績・DRM・Overlay APIを統合しない。
  SDK統合はSteam配布の必須条件ではない。
- LinuxはSteam Linux Runtime環境で依存ライブラリとsandboxを確認する。
  `--no-sandbox` を配布用の回避策にしない。
- Steam DeckのVerified判定は別審査。Linux版が生成できたことだけでは対応完了にならない。
  コントローラだけで操作できること、文字の可読性、画面表示等を実機で確認する。
- Steamのビルド審査前に、ストアで宣言したOS・機能と動作が一致することを確認する。

[Steamworks API](https://partner.steamgames.com/doc/sdk/api) /
[Overlay](https://partner.steamgames.com/doc/features/overlay) /
[Linux開発](https://partner.steamgames.com/doc/store/application/platforms/linux) /
[Steam Deck互換性](https://partner.steamgames.com/doc/steamhardware/compat) /
[ビルド審査](https://partner.steamgames.com/doc/store/review_process)

## English

Local exports do not require Steamworks, SteamCMD or distribution signing credentials.

### STEP1: Prepare target environments and icons

- Build macOS Universal applications and perform macOS signing/notarization on macOS.
- Prepare an environment for each distributed OS to verify launch, input and saved data.
- Place Windows ICO, macOS ICNS and Linux PNG icons in the game's `src/assets/icons/`.
  Replace template placeholders before distribution. STEP3 documents omitted icon settings.

#### Supported operating systems and architectures

| OS | Default | Accepted `architectures` / `--arch` values |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

32-bit Windows (`ia32`) is not supported. `win32` is Electron's internal OS name for Windows, not a CPU bitness indicator.
For example, `My Game-win32-x64/` is a 64-bit Windows package.
Use `windows` or `steam:windows` for `--platform`.

macOS `universal` produces one `.app` containing both x64 and arm64.

### STEP2: Prepare the Steamworks app, depots and testing branch (Steam distribution)

1. Find the issued App ID in the app's Steamworks administration page.
2. Create depots in SteamPipe > Depots and select their operating systems. Use All languages for shared language content,
   and include the depots in development and retail Packages. **Use actual Depot IDs; do not infer them from the App ID.**
3. Create an `internal` branch in SteamPipe > Builds and **set a password shared only with the intended testers.**
   The name alone does not make it private. The builder does not create branches, configure passwords or verify protection.
4. Give testers access to the game and its depots. For external testers of an unreleased game, use Release State Override
   (beta) keys or another appropriate method. A branch password alone does not grant game access.
   Keep the password out of JSON and the repository, and ask testers not to share it with others.

Separate OS depots and shared depots are supported. Assign the same Depot ID to operating systems that share a depot,
and set its Steamworks OS selection accordingly, such as All OSes.
Shared depots distribute every OS's files and increase download size. Valve recommends separate depots for OS-specific files.

[Depot configuration](https://partner.steamgames.com/doc/store/application/depots) /
[Beta branches](https://partner.steamgames.com/doc/store/application/branches) /
[Testing on Steam](https://partner.steamgames.com/doc/store/testing) /
[Release State Override keys](https://partner.steamgames.com/doc/features/keys)

### STEP3: Configure electron.config.json

Manage game-specific settings in `electron.config.json` at the project root.
Both JavaScript and TypeScript templates include it; replace the defaults with your game's values.
`appId` is the macOS bundle ID and the identifier used for the save location; `steam.appId` is Valve's numeric App ID.
These are separate identifiers. The game version comes from the root `package.json`.

```json
{
  "appId": "app.example.game",
  "appName": "My Game",
  "executableName": "my-game",
  "companyName": "Example Company",
  "icons": {
    "windows": "src/assets/icons/icon.ico",
    "macos": "src/assets/icons/icon.icns",
    "linux": "src/assets/icons/icon.png"
  },
  "architectures": { "windows": "x64", "macos": "universal", "linux": "x64" },
  "window": { "width": 1280, "height": 720, "fullscreen": false },
  "steam": {
    "appId": null,
    "branch": "internal",
    "depots": { "windows": null, "macos": null, "linux": null }
  },
  "macos": { "sign": false, "notarize": false }
}
```

| Field | Purpose / behavior | Defaults / notes |
|---|---|---|
| `companyName` | Company name in the Windows executable metadata. | Falls back to `author.name` in `package.json`, then to the game name. |
| `appName` / `executableName` | `appName` is the window and application name. `executableName` is the executable file name. | The display name and executable name can be set independently. |
| `description` | Optional description used in the generated Electron host's `package.json` and the Windows file description. | The JSON value takes priority. If omitted or `null`, uses `description` from the root `package.json`. If both are absent, uses an empty string. An explicit `""` also remains empty. |
| `icons` | Icon paths, relative to the project root or absolute. Use ICO for Windows, ICNS for macOS and PNG for Linux. | Each key is optional. Omitted platforms use the builder's placeholder Next2D icons; `"icons": {}` uses placeholders for every OS. A specified file that does not exist causes an error. |
| `architectures` | CPU architecture per OS. See STEP1 for supported operating systems and architectures. | Defaults to `x64` for Windows/Linux and `universal` for macOS. `--arch` takes priority. |
| `steam.appId` | Valve's numeric App ID. Set to `null` if it has not been issued. | Local exports still work without an App ID, but SteamPipe VDFs are not generated. |
| `steam.branch` | Target beta branch for `--steam-upload`. | Defaults to `internal`. `--steam-branch` takes priority. Create the branch and configure its password in Steamworks. |
| `steam.depots` | Actual Depot IDs created in Steamworks. Assigning the same ID to multiple operating systems combines them in one depot. | Do not infer Depot IDs from the App ID. If `null` or omitted, only the application and launch metadata are generated, without upload VDFs. |
| `macos` | macOS signing and notarization settings. | Use `false` for both `sign` / `notarize` during local testing. Enable both for production distribution. |

### STEP4: Prepare signing and upload credentials (distribution)

#### macOS signing and notarization

Install a Developer ID Application certificate in the macOS keychain and save a notarization profile using
`xcrun notarytool store-credentials`. Do not put credentials in JSON.

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
```

For distribution, set both `macos.sign` / `macos.notarize` to `true`.
Alternatively, prefix the STEP5 macOS command with `NEXT2D_STEAM_RELEASE=1` to require both even when JSON specifies false.
Missing credentials, signing failures or notarization failures fail the build.

Steam uses the `darwin` target. Valve requires 64-bit support and Apple notarization for new macOS applications.
The template entitlements include library validation / DYLD settings for JIT and Steam Overlay,
without `com.apple.security.app-sandbox` (which is separate from Chromium's renderer sandbox).
[Valve platform requirements](https://partner.steamgames.com/doc/store/application/platforms) /
[Electron Packager signing and notarization options](https://electron.github.io/packager/main/interfaces/Options.html)

#### SteamCMD and the build account

Obtain SteamCMD for your host OS from the [Steamworks SDK ContentBuilder](https://partner.steamgames.com/doc/sdk/uploading).
Use `steamcmd.exe` on Windows or `steamcmd.sh` on macOS / Linux.
The builder uses `steamcmd` on PATH (`steamcmd.exe` on Windows), or the executable path set in `STEAMCMD`.
Specify only the path, without arguments or a shell command. A `.sh` file needs executable permission.
Install any OS libraries required by SteamCMD according to its setup instructions.

Grant the upload account `Edit App Metadata` and `Publish App Changes To Steam` permissions for the target app.
Example for macOS / Linux; replace the path and account name:

```sh
export STEAMCMD="/absolute/path/to/steamcmd.sh"
export STEAM_USERNAME="your_build_account"
"$STEAMCMD" +login "$STEAM_USERNAME" +quit
```

During the first login, enter the password and Steam Guard code interactively in SteamCMD.
The builder then reuses that SteamCMD installation's saved login without passing a password as a command-line argument.
If authentication expires, log in again with the same SteamCMD. Builder uploads fail instead of waiting for interactive input.
In CI, restore and manage SteamCMD's `config/config.vdf` as a Secret and exclude it from build artifacts.
In Windows PowerShell, set `$env:STEAMCMD` and `$env:STEAM_USERNAME`.

### STEP5: Export each OS and collect the packages

Run the required OS commands from the game root. Exporting does not upload or publish anything.

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

Append an option such as `--arch arm64` to change the architecture.
Template scripts `npm run build:steam:windows` / `build:steam:macos` / `build:steam:linux` perform the same operations
and already specify `--env prd`. Pass npm arguments with `-- --arch arm64` or `-- --env dev`.

The default output location is `dist/steam/<OS>/build/<env>/`.

| OS | Entire folder to distribute | Separate depot Executable | Shared depot Executable |
|---|---|---|---|
| Windows | `My Game-win32-x64/` | `my-game.exe` | `windows/my-game.exe` |
| macOS | `My Game-darwin-universal/` | `My Game.app` | `macos/My Game.app` |
| Linux | `My Game-linux-x64/` | `my-game` | `linux/my-game` |

Read the actual launch path, architecture and ContentRoot from `*-steampipe/launch.json`.
Distribute the entire folder, including Electron libraries, locales, licenses and resources.
Steam needs no DMG/DEB/MSI installer, and no additional application build is required after export.

For separate machines or CI, collect each OS's `build/<env>/`, including `steam-package.json` and `*-steampipe/`,
under the same `dist/steam/<OS>/build/<env>/` layout. Transfer CI artifacts as `tar.gz` to preserve macOS symlinks
and Linux executable permissions. Use STEP6's `--steam-root` for a custom output location.

Re-export affected operating systems after changing configuration or version. Even with an unchanged version number,
keep all OS packages on the intended revision. When exporting x64 and arm64 successively for one OS, the last successful
architecture is used. Combining multiple architectures of one OS into a shared depot is not supported.

Verify signing, notarization and stapling for macOS distribution packages (adjust the paths):

```sh
codesign --verify --deep --strict 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
```

### STEP6: Configure launching, upload and test

Add OS-specific Launch Options in Steamworks under Installation > General Installation.
Use the Executable from STEP5's `launch.json`, leaving Arguments/Working Directory empty.
Set the 64-bit condition for x64. Match supported OS versions and minimum requirements to the bundled Electron and hardware testing,
then Publish the Steamworks changes.

Run these commands from the game root. Shared and separate depots are uploaded together in a single AppBuild.
No Web/Electron rebuild or prior `--steam-manifest` command is needed.

```sh
# Local validation only: no SteamCMD, credentials or Steam connection required
npx @next2d/builder --steam-upload --env prd --dry-run

# After validation, upload to the configured branch
npx @next2d/builder --steam-upload --env prd
```

| Option / setting | Purpose |
|---|---|
| `--steam-branch internal` | Overrides `steam.branch`; requests activation through `SetLive`. `default` is not allowed. Switch public release builds through Steamworks. |
| `--steam-root dist/steam` | Collected package location; defaults to `dist/steam`. Accepts an absolute path or a path relative to the game root. Specify custom output locations because uploading does not read the Vite configuration. |
| `--dry-run` | Validates packages against configuration. Does not check Steam permissions, branches or passwords. |
| Incompatible options | `--build` / `--preview` / `--open` / `--steam-manifest` / `--arch`. `--platform` is also unnecessary. |

The corresponding template npm scripts are `upload:steam:check` / `upload:steam`.

The builder checks `steam-package.json` and executables for every OS configured in `steam.depots`.
Missing or inconsistent App IDs, Depot IDs, versions, game names or architectures stop the operation before SteamCMD starts.
VDFs are regenerated from validated packages; ordinary export VDFs remain unchanged.

| Location (under `--steam-root`) | Contents |
|---|---|
| `uploads/<env>/<branch>-<identifier>/` | VDFs and `upload-plan.json`. Dry runs use `Preview=1` without `SetLive`. Successful uploads record the BuildID in `upload-result.json`. |
| `uploads/<env>/cache/<branch>/` | SteamPipe logs and incremental upload cache. |

On the Steamworks Builds page, verify that the recorded BuildID is the target branch's current build.
Steam may request additional confirmation depending on the account or app state.
Testers enter the password and select `internal` in the Steam client's Properties > Game Versions & Betas.
Verify installation and launch on each OS, including both Intel and Apple Silicon for macOS Universal.

### Reference: VDF generation and manual uploading

Skip these operations when using the builder in STEP6. This section is for direct SteamCMD use.

| Layout | VDF output and conditions during ordinary export |
|---|---|
| Separate OS depots | Each OS's `*-steampipe/` directory. |
| Shared depots | `dist/steam/shared/<env>/depot-<ID>/` once all required OS packages exist. Missing operating systems are listed in `launch.json`; neither upload VDFs nor standalone per-OS VDFs are generated for an incomplete group. |

Some operating systems may share a Depot ID while others use separate depots. `FileMapping` assigns OS subdirectories without copying assets again.
To regenerate only shared depot VDFs after collecting packages from separate machines:

```sh
npx @next2d/builder --platform steam:macos --env prd --steam-manifest
```

The npm alias is `build:steam:manifest` (append `-- --env dev` for another environment). This command runs on any OS.
`--platform` selects the Vite configuration; all configured shared depots are combined.
It selects the last successful exports using `build.outDir` and `--env`, validating the App ID, Depot IDs, game name, executable name and version.
Missing packages cause exit code 1. Old combined VDFs are invalidated during regeneration.
Do not combine this command with `--arch` / `--preview` / `--build` / `--open`.

`app_preview.vdf` validates file mappings, `app_build.vdf` uploads, and `depot_build.vdf` recursively maps folder contents.
`steam_appid.txt` is excluded from distribution. ContentRoot is relative to the VDF, so move a standalone depot's application
and `*-steampipe/` together; preserve the whole `dist/steam/` layout for shared depots.

```text
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_preview.vdf" +quit
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_build.vdf" +quit
```

Ordinary exports and `--steam-manifest` omit `SetLive`; assign the uploaded build to its branch through Steamworks.
Each per-OS VDF updates one depot, so verify the final Build references the intended manifest version for every OS.
[Official SteamPipe workflow and VDF specification](https://partner.steamgames.com/doc/sdk/uploading)

### Reference: Templates, host and migration

`create-next2d-app` initializes `appId`, `appName`, `executableName` and `companyName` from the project name.
For example, `my-game` produces `app.example.my-game` for `appId` and `my-game` for the other three fields.
Replace the bundle ID and company name with your own values before distribution.
Missing CPU settings receive the defaults for each OS; values already provided by the template are preserved.
Other settings, such as icon paths and Steam IDs, are also preserved.

The builder manages Electron host code. It stages the host, runtime configuration and web assets in an OS temporary directory,
then removes it after export, including failures. No `electron/`, Electron-specific `node_modules` or lockfile is created in the game.
Electron is pinned in `templates/electron/package.json`; Packager is pinned in `src/tool-packages.ts`. Both are downloaded and cached.
Packager is acquired only for Electron exports and handles OS packages, icons, signing and notarization.
The host has no npm dependencies or native addons, so ABI-specific rebuilding is unnecessary.
Preview exports for the host OS/CPU and launches the application under `dist/<platform>/build/<env>/`.

Legacy `electron/` host code, `config.forge` and custom npm dependencies are not used.
For migration, move icons into shared assets and update their JSON paths before deleting the old directory.
Custom main/preload code or native addons require builder support before migration.

The template host loads assets using absolute paths and the fixed origin `next2d://game`.
Local fetch, Workers and localStorage work independently of the startup working directory.
F11/Alt+Enter enters fullscreen; Escape exits it. Closing the window also terminates the process on macOS.
Node integration is disabled, context isolation and sandboxing are enabled, and external navigation and new windows are blocked.
A Next2D CSP is applied. Games using external APIs must explicitly add their destinations.
[Electron security](https://www.electronjs.org/docs/latest/tutorial/security) /
[protocol](https://www.electronjs.org/docs/latest/api/protocol)

Save data is stored in the `appId` directory under Electron's appData location.
Changing the display name does not change that location. Saves from older file-origin development builds are not migrated automatically.
Steam Cloud is not integrated. When adding it, design game-specific save files and per-account storage separately,
rather than syncing the entire Chromium profile.
[Steam Cloud](https://partner.steamgames.com/doc/features/cloud)

### Pre-release validation

In addition to STEP6's installation and launch checks, verify:

- Exit behavior, offline launch and saved data after updates.
- Mouse/keyboard/controller input, resolution and fullscreen switching, audio and sleep/resume.
- Shift+Tab Overlay. Electron uses multiple processes, so verify actual behavior on each OS/GPU.
  This export does not integrate the Steamworks SDK, achievements, DRM or Overlay APIs.
  SDK integration is not required for Steam distribution.
- On Linux, check dependent libraries and sandboxing in the Steam Linux Runtime environment.
  Do not use `--no-sandbox` as a distribution workaround.
- Steam Deck Verified requires a separate review. Producing a Linux package alone does not establish compatibility.
  Test controller-only operation, text readability and display behavior on hardware.
- Before Steam's build review, verify that the supported operating systems and features advertised on the store match the application.

[Steamworks API](https://partner.steamgames.com/doc/sdk/api) /
[Overlay](https://partner.steamgames.com/doc/features/overlay) /
[Linux development](https://partner.steamgames.com/doc/store/application/platforms/linux) /
[Steam Deck compatibility](https://partner.steamgames.com/doc/steamhardware/compat) /
[Build review](https://partner.steamgames.com/doc/store/review_process)
