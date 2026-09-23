# Electron / Steam 書き出し / Export

[日本語](#日本語) | [English](#english)

## 日本語

### 設定

プロジェクトルートの `electron.config.json` でゲーム固有設定を管理する。
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
| `architectures` | OSごとのCPU設定。対応する値と指定例は後述の「対応OS・CPU」を参照。 | 省略時はWindows/Linuxが `x64`、macOSが `universal`。`--arch` が優先する。 |
| `steam.appId` | Valveの数値App ID。未発行なら `null` を指定する。 | 未発行でもローカル書き出しは可能。SteamPipe用VDFは生成しない。 |
| `steam.branch` | `--steam-upload` の反映先betaブランチ。 | 省略時は `internal`。`--steam-branch` が優先する。ブランチの作成・パスワード設定はSteamworks側で行う。 |
| `steam.depots` | Steamworksで作成した実際のDepot ID。複数OSに同じIDを指定すると、同一Depotへまとめて配布する。 | App IDから推測しない。`null` / 未指定ならアプリと起動情報だけを生成し、アップロード用VDFは生成しない。 |
| `macos` | macOSアプリの署名・公証設定。 | ローカル検証用は `sign` / `notarize` ともに `false`。本番配布では両方を有効化する。 |

#### テンプレートとApp作成時の初期値

JavaScript / TypeScriptの両テンプレートは `electron.config.json` と
`src/assets/icons/` の仮のNext2Dアイコン（ICO / ICNS / PNG）を含む。
配布前にアイコンとゲーム固有の値を差し替える。Steam App ID / Depot IDの初期値は `null`。

`create-next2d-app` はプロジェクト名から `appId`、`appName`、`executableName`、
`companyName` を設定する。例えば `my-game` なら `appId` は `app.example.my-game`、
他の3項目は `my-game` になる。配布前にbundle IDと会社名を自分の値へ変更する。
CPU設定が省略されている場合はOSごとの既定値を補い、テンプレートに指定済みの値は保持する。
アイコンパスやSteam IDなどの設定も保持する。

#### 対応OS・CPU

| OS | 既定値 | `architectures` / `--arch` の対応値 |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

32bit版Windows（`ia32`）には対応しない。`win32` はElectron内部のWindowsのOS名で、
32bitを意味しない。例えば `My Game-win32-x64/` は64bit版Windows向けの成果物。
`--platform` には `windows` または `steam:windows` を指定する。

macOSの `universal` はx64とarm64を含む1つの `.app` で、macOS上で生成する。
Windows/Linuxの既定値はx64。ARM64版は `architectures.windows` / `architectures.linux`
に `arm64` を設定するか、書き出し時に `--arch arm64` を指定する。

```sh
npx @next2d/builder --platform steam:windows --env prd --arch arm64
```

#### 書き出しコマンド

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
npx @next2d/builder --platform macos --env prd --preview
```

更新済みのテンプレートと `create-next2d-app` が用意するnpmコマンドでも実行できる。

```sh
npm run build:steam:windows
npm run build:steam:macos
npm run build:steam:linux
npm run build:steam:windows -- --arch arm64
npm run build:steam:windows -- --env dev
```

これらのnpmコマンドは `--env prd` を指定済み。末尾の `-- --env dev` などで上書きできる。
builderを直接呼ぶ場合は `--env` を指定する。
書き出しコマンドはSteamへのアップロードや公開を自動では行わない。

#### builderが管理するElectronホスト

Electron関連でゲーム側に必要な設定ファイルは `electron.config.json` だけ。
ゲーム名・バージョン・説明はルートの `package.json` と合わせてホストへ反映する。
説明は `electron.config.json` の `description` を優先する。
アイコン画像はゲームの共通アセットを参照でき、未指定ならbuilderの仮アイコンを使う。

builderはOSの一時ディレクトリにホスト・runtime設定・Web資産を配置し、書き出し後に削除する。
ゲームルートへ `electron/` やElectron用の `node_modules` / lockfileを作らない。
失敗時も一時ホストを削除する。Electronバージョンはbuilderの
`templates/electron/package.json` で固定し、Packagerが実行ファイルをダウンロード・キャッシュする。
ゲーム側でElectronをインストールする処理は不要。

`@electron/packager` もbuilderのnpm依存には含めず、Electronの書き出し時だけ `npx` で取得する。
バージョンは `src/tool-packages.ts` で固定する。npm/npxを利用できるNode.js環境が必要で、
初回取得にはネットワーク接続が必要。取得済みツールはnpmキャッシュを再利用する。
Web / Xboxビルドや `--steam-manifest` だけの実行ではPackagerを取得しない。
PackagerはOS別パッケージの生成、アイコンの適用、署名・公証、Electron実行ファイルの取得を担当する。
生成するホストにはnpm依存やネイティブアドオンがないため、ABIに合わせた再ビルドは不要。

プレビューも実行ホストのOS/CPU向けアプリを書き出して起動する。
一時ホストの削除後、`dist/<platform>/build/<env>/` のアプリを実行するので、
本番書き出しと同じホスト・資産読み込みを確認できる。

旧 `electron/` のホストコード・`config.forge`・独自npm依存は参照しない。
移行時はアイコンを共通アセットへ移し、設定パスを更新してから `electron/` を削除する。
独自のmain/preloadやネイティブアドオンを持つプロジェクトは、移行前にbuilder側への対応が必要。

builder自体を開発中なら、ゲームのdevDependencyに `"@next2d/builder": "file:../builder"` を指定し、
builder→ゲームの順に `npm ci` する。builder側のprepareでコンパイルされる。
builderのソース変更後はbuilderで `npm run build` を実行する。
公開後はこの依存を公開バージョンへ変更でき、ゲームのコマンドはそのまま使える。

### 成果物と起動

`dist/steam/<OS>/build/<env>/` に以下を生成する（デフォルトoutDirの場合）。
下表のExecutableはOS別の独立したDepotを使う場合。共有Depotの場合は後述のOS名プレフィックスが付く。

| OS | 配布するフォルダ全体 | SteamworksのExecutable |
|---|---|---|
| Windows | `My Game-win32-x64/` | `my-game.exe` |
| macOS | `My Game-darwin-universal/` | `My Game.app` |
| Linux | `My Game-linux-x64/` | `my-game` |

`*-steampipe/launch.json` に実際の起動パス・CPU・ContentRootが記録される。
実行ファイルだけを取り出さず、Electronのライブラリ、locales、ライセンス、resourcesを含む
フォルダ全体を配布する。Steamにはインストーラ（DMG/DEB/MSI）は不要。
macOSの `.app` 内のシンボリックリンクとLinuxの実行権限を保持する。
CIの転送は `tar.gz` にしてからartifactへ格納する。

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

### 同じDepotを複数OSで使う場合

`steam.depots` の同じIDを共有するOSを1グループとして扱う。例えば全OSに同じDepot IDを
指定すると、Steamのインストール先を次のように分け、Electronの同名ファイルの衝突を避ける。

| OS | SteamworksのExecutable（例） |
|---|---|
| Windows | `windows/my-game.exe` |
| macOS | `macos/My Game.app` |
| Linux | `linux/my-game` |

Steamworks側のDepot対象OSを共有するOSまたはAll OSesに設定し、Launch Optionには
生成された `launch.json` のExecutableを指定する。OS別の起動条件も設定する。
同一Depot内の全OS分のファイルが配布されるため、OS別Depotよりダウンロード容量は増える。
ValveはOS固有ファイルには別Depotを推奨しているが、OS共有自体は禁止していない。
[公式DepotのOS設定](https://partner.steamgames.com/doc/store/application/depots)

各OSの書き出しは単独で成功する。共有対象の全OSの成果物が揃うと自動で
`dist/steam/shared/<env>/depot-<ID>/` に `app_build.vdf`、`app_preview.vdf`、
`depot_build.vdf`、`launch.json` を生成する。未完了なら `launch.json` に不足OSを記録し、
アップロード用VDFは生成しない。共有DepotのOS単独VDFも生成しない。

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

統合VDFは各OSのパッケージを `FileMapping` で `windows/` / `macos/` / `linux/` へ配置する。
資産の再コピーは行わず、元パッケージ内のmacOSシンボリックリンク・Linux実行権限を保持する。
VDFだけではなく `dist/steam/` 全体のディレクトリ構成を保持して移動すること。
[公式FileMapping仕様](https://partner.steamgames.com/doc/sdk/uploading)

CI等で別のマシン上で書き出した場合は、各OSの `build/<env>/` を
`steam-package.json` と `*-steampipe/` を含めて同じ `dist/steam/<OS>/build/<env>/` に集め、次を実行する。

```sh
npx @next2d/builder --platform steam:macos --env prd --steam-manifest
```

更新済みのテンプレートと `create-next2d-app` では、同じ処理を次のnpmコマンドで実行できる。

```sh
npm run build:steam:manifest
# prd以外の成果物を統合する場合
npm run build:steam:manifest -- --env dev
```

このコマンドはWebビルドやElectron書き出しを行わず、共有DepotのVDFだけを再生成する。
どのOS上でも実行できる。`--platform` はVite設定を読むためのSteamターゲットで、統合対象は
設定内のすべての共有Depot。共有対象のパッケージが不足していれば終了コード1になる。
`build.outDir` と `--env` を使い、各OSの最後に成功した書き出しを選ぶ（`--arch` 指定も記録される）。
同じOSのx64版とarm64版を順に書き出した場合、統合対象になるのは最後に成功したCPUの成果物。
1つの共有Depotへ同じOSの複数CPU版を同時にまとめる処理は行わない。
`--steam-manifest` は `--arch` / `--preview` / `--build` / `--open` と併用できない。
App ID・Depot ID・ゲーム名・実行ファイル名・バージョンが現設定と一致する必要がある。
同じバージョン番号で更新する場合も、配布する全OSを意図したリビジョンで揃えること。

一部OSだけIDを共有する構成にも対応する。共有しないOSは従来どおり単独VDFを生成する。
設定やバージョンを変更した場合は対象OSを再度書き出す。古い統合VDFは再生成時に無効化する。

### macOSの配布用署名・公証

Steam用は `darwin` であり、Mac App Store向けの `mas` ではない。
Valveは新規macOSアプリに64bitとAppleの公証を要求している。
テンプレートのentitlementsはJITとSteam Overlayに必要なlibrary validation / DYLD設定を含み、
`com.apple.security.app-sandbox` は付けない（Chromiumのrenderer sandboxとは別）。
[Valveのプラットフォーム要件](https://partner.steamgames.com/doc/store/application/platforms)

macOS上でDeveloper ID Application証明書をキーチェーンへ登録し、Appleの
`xcrun notarytool store-credentials` で公証用プロファイルを保存してから実行する。
認証情報はJSONへ書かない。

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
NEXT2D_STEAM_RELEASE=1 npx @next2d/builder --platform steam:macos --env prd
```

`NEXT2D_STEAM_RELEASE=1` はJSONのfalse指定に関わらず署名と公証を必須にする。
またはJSONの `macos.sign` / `macos.notarize` を両方trueにする。
認証情報不足・署名失敗・公証失敗はビルド失敗となる。
署名、公証、staple後にSteamPipeへ渡す。確認例:

```sh
codesign --verify --deep --strict 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
```

[Electron Packagerの署名・公証設定](https://electron.github.io/packager/main/interfaces/Options.html)

### Steamworks / SteamPipe

1. Steamworks > SteamPipe > Depotsで配布構成に合わせたDepotを作成し、対象OSを指定する。
   OS別Depotと、複数OS向けの同一Depotのどちらも使用できる。
   言語共通ならAll languages。開発用・販売用Packageにも各Depotを含める。
2. Installation > General InstallationにOS別Launch Optionを追加する。
   Executableには生成された `launch.json` の値を指定。Arguments/Working Directoryは空欄。
   x64版は64bit条件を指定し、macOS UniversalはIntel/Apple Silicon両方でテストする。
3. 変更をSteamworksでPublishする。配布したいOS・最低OS要件は、同梱するElectronの
   対応バージョンと実機テストに合わせる（古いSteamページの最低OS表だけで決めない）。
4. JSONへApp ID/Depot IDを設定して再ビルド。単独Depotは `*-steampipe/`、
   共有Depotは全対象OSが揃った時点で `shared/<env>/depot-<ID>/` に次のVDFが出る。
   `app_preview.vdf`: ファイルマッピング検証。`app_build.vdf`: アップロード。
   `depot_build.vdf`: フォルダ内容をインストールルートへ再帰マッピングする。
   `steam_appid.txt` は配布対象から除外する。
5. Steamworks SDKのContentBuilder/SteamCMDを取得し、権限のあるビルドアカウントでログインする。
   パスワードはコマンド・リポジトリへ保存しない。

```text
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_preview.vdf" +quit
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_build.vdf" +quit
```

ContentRootはVDFからの相対パス。単独Depotはアプリと `*-steampipe/` をセットで、
共有Depotは `dist/steam/` 全体の構成を保持して移動する。
通常の書き出し・`--steam-manifest` が生成するVDFにはSetLiveを入れていない。アップロード後にSteamworksでパスワード付きbetaへ設定し、
各OSのSteamクライアントからインストールして検証する。
OSごとのVDFは1つのDepotを更新するため、最終BuildのDepot manifestが全OSで意図した版になっているか確認する。
[SteamPipe公式手順・VDF仕様](https://partner.steamgames.com/doc/sdk/uploading) /
[Depots](https://partner.steamgames.com/doc/store/application/depots)

### builderからアップロードしてinternalでテストする

`--steam-upload` は書き出し済みの成果物をSteamCMDで一括アップロードし、
`SetLive` で指定したbetaブランチへの反映を要求する。Web/Electronの再ビルドは行わない。
`--platform` は不要で、`--build` / `--preview` / `--open` / `--steam-manifest` / `--arch` とは併用しない。
builderのnpm依存は追加していない。SteamCMDは別途用意する。

#### Steamworksで関係者向けの配布先を用意する

1. Steamworksのアプリ管理画面 > SteamPipe > Buildsで `internal` ブランチを作成する。
2. **ブランチにパスワードを設定し、関係者だけに共有する。** `internal` という名前だけでは非公開にならない。
   builderはブランチの作成やパスワード設定・確認を行わない。パスワードはJSONやリポジトリに保存しない。
3. テスターがゲームと対象Depotを利用できるPackageのライセンスを持っていることを確認する。
   未発売ゲームの外部テスターにはRelease State Override（beta）キー等を使用する。
   ブランチのパスワードだけではゲームの利用権は付与されない。
4. 関係者のSteamクライアントで、ゲームのプロパティ > ゲームバージョンとベータから
   パスワードを入力して `internal` を選ぶ。

`default` ブランチはこのコマンドでは指定できない。一般公開用ビルドの切り替えはSteamworksで行う。
betaのパスワードは共有秘密なので、第三者に渡ればアクセスできる。個々のテスターの利用権も管理する。

[公式betaブランチ設定](https://partner.steamgames.com/doc/store/application/branches) /
[Steamでのテスト](https://partner.steamgames.com/doc/store/testing) /
[Release State Overrideキー](https://partner.steamgames.com/doc/features/keys)

#### SteamCMDの準備と認証

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

#### 検証とアップロード

`electron.config.json` の `steam.depots` に設定された全OSの成果物を揃え、ゲームのルートで実行する。
同じDepot IDを使うOSはOS別サブディレクトリへまとめ、異なるDepotも1回のAppBuildでアップロードする。

```sh
# ローカル検証だけ。SteamCMDや認証は不要、Steamへの接続・送信も行わない
npx @next2d/builder --steam-upload --env prd --dry-run

# アップロードし、steam.branch（省略時internal）へ反映
npx @next2d/builder --steam-upload --env prd

# ブランチや成果物の収集先を明示する場合
npx @next2d/builder --steam-upload --env prd --steam-branch internal --steam-root dist/steam
```

ローカル開発中は `npx @next2d/builder` を `node ../builder/dist/index.js` に置き換える。
この機能を含むbuilderの公開後は、ゲーム側の `npm run upload:steam:check` / `npm run upload:steam` も使用できる。

`--steam-root` はゲームルートからの相対パス、または絶対パス。既定は `dist/steam`。
アップロード処理ではVite設定を読み込まないため、独自の出力先を使う場合はこの引数を指定する。
成果物の `steam-package.json` にあるApp ID・Depot ID・バージョン・ゲーム名・CPUと実行ファイルを検証し、
不足や不整合があればSteamCMDを起動する前に停止する。VDFは既存ファイルを流用せず検証結果から再生成する。

各実行のVDFと `upload-plan.json` は `dist/steam/uploads/<env>/<branch>-<識別子>/` に保存する。
dry-runでは `Preview=1` のVDFを生成して `SetLive` を付けない。これはローカル検証であり、
SteamCMDの認証・サーバー側の権限・ブランチ・パスワード設定が正しいことを保証するものではない。
通常の書き出し用VDFは変更しない。SteamPipeのログ・差分キャッシュは `uploads/<env>/cache/<branch>/` に置く。

アップロード時は終了コードとSteamCMDの成功メッセージを確認し、BuildIDを `upload-result.json` に記録する。
SteamworksのBuilds画面で、記録されたBuildIDが `internal` の現在のビルドになったことを確認する。
アカウントやアプリの状態によって、Steam側で追加の確認が必要になる場合がある。

### リリース前の確認範囲

- 対象OSのSteamからインストール・起動・終了、オフライン起動、更新後の保存データ。
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

### Configuration

Manage game-specific settings in `electron.config.json` at the project root.
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
| `architectures` | CPU architecture per OS. See “Supported operating systems and architectures” below for accepted values and examples. | Defaults to `x64` for Windows/Linux and `universal` for macOS. `--arch` takes priority. |
| `steam.appId` | Valve's numeric App ID. Set to `null` if it has not been issued. | Local exports still work without an App ID, but SteamPipe VDFs are not generated. |
| `steam.branch` | Target beta branch for `--steam-upload`. | Defaults to `internal`. `--steam-branch` takes priority. Create the branch and configure its password in Steamworks. |
| `steam.depots` | Actual Depot IDs created in Steamworks. Assigning the same ID to multiple operating systems combines them in one depot. | Do not infer Depot IDs from the App ID. If `null` or omitted, only the application and launch metadata are generated, without upload VDFs. |
| `macos` | macOS signing and notarization settings. | Use `false` for both `sign` / `notarize` during local testing. Enable both for production distribution. |

#### Templates and defaults for new apps

Both the JavaScript and TypeScript templates include `electron.config.json` and placeholder Next2D icons
(ICO / ICNS / PNG) in `src/assets/icons/`. Replace the icons and game-specific values before distribution.
Steam App ID / Depot IDs initially use `null`.

`create-next2d-app` initializes `appId`, `appName`, `executableName` and `companyName` from the project name.
For example, `my-game` produces `app.example.my-game` for `appId` and `my-game` for the other three fields.
Replace the bundle ID and company name with your own values before distribution.
Missing CPU settings receive the defaults for each OS; values already provided by the template are preserved.
Other settings, such as icon paths and Steam IDs, are also preserved.

#### Supported operating systems and architectures

| OS | Default | Accepted `architectures` / `--arch` values |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

32-bit Windows (`ia32`) is not supported. `win32` is Electron's internal OS name for Windows, not a CPU bitness indicator.
For example, `My Game-win32-x64/` is a 64-bit Windows package.
Use `windows` or `steam:windows` for `--platform`.

macOS `universal` produces one `.app` containing both x64 and arm64, and must be built on macOS.
Windows/Linux default to x64. For ARM64, set `architectures.windows` / `architectures.linux` to `arm64`
or pass `--arch arm64` when exporting.

```sh
npx @next2d/builder --platform steam:windows --env prd --arch arm64
```

#### Export commands

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
npx @next2d/builder --platform macos --env prd --preview
```

The updated templates and `create-next2d-app` also provide these npm scripts:

```sh
npm run build:steam:windows
npm run build:steam:macos
npm run build:steam:linux
npm run build:steam:windows -- --arch arm64
npm run build:steam:windows -- --env dev
```

These npm scripts already specify `--env prd`; append an override such as `-- --env dev` when needed.
Specify `--env` when invoking the builder directly.
Export commands do not automatically upload to or publish on Steam.

#### The Electron host managed by the builder

The only Electron-specific configuration file needed in the game is `electron.config.json`.
The builder combines it with the root `package.json` to populate the host's game name, version and description.
For the description, `description` in `electron.config.json` takes priority.
Icons may reference shared game assets; unspecified icons use the builder's placeholders.

The builder stages the host, runtime configuration and web assets in an OS temporary directory and deletes it after export.
It does not create an `electron/` directory or Electron-specific `node_modules` / lockfile in the game root.
The temporary host is also removed on failure. The Electron version is pinned in the builder's
`templates/electron/package.json`, and Packager downloads and caches the runtime.
The game does not need a separate Electron installation step.

`@electron/packager` is not an npm dependency of the builder either; it is acquired through `npx` only for Electron exports.
Its version is pinned in `src/tool-packages.ts`. Node.js with npm/npx is required, and the initial download needs network access.
Downloaded tools are reused from npm's cache. Web / Xbox builds and standalone `--steam-manifest` runs do not acquire Packager.
Packager handles OS-specific packages, icons, signing/notarization and Electron runtime downloads.
The generated host has no npm dependencies or native addons, so rebuilding for a specific ABI is unnecessary.

Preview also exports an application for the host OS/CPU and launches it.
After the temporary host has been removed, it runs the application in `dist/<platform>/build/<env>/`,
allowing you to test the same host and asset loading used by production exports.

Legacy host code, `config.forge` and custom npm dependencies under `electron/` are not used.
When migrating, move icons into shared assets and update their paths before deleting `electron/`.
Projects with a custom main/preload or native addons need support in the builder before migration.

When developing the builder itself, you can set `"@next2d/builder": "file:../builder"` in the game's devDependencies
and run `npm ci` in the builder, then in the game. The builder's prepare script compiles its source.
After changing builder source files, run `npm run build` in the builder.
After publication, this dependency can point to a published version while keeping the same game commands.

### Output and launching

With the default outDir, packages are generated under `dist/steam/<OS>/build/<env>/`.
The Executable values below apply to separate depots for each OS. Shared depots add the OS prefix described later.

| OS | Entire folder to distribute | Steamworks Executable |
|---|---|---|
| Windows | `My Game-win32-x64/` | `my-game.exe` |
| macOS | `My Game-darwin-universal/` | `My Game.app` |
| Linux | `My Game-linux-x64/` | `my-game` |

`*-steampipe/launch.json` records the actual launch path, CPU architecture and ContentRoot.
Distribute the entire folder, including Electron libraries, locales, licenses and resources, rather than extracting only the executable.
Steam does not require an installer (DMG/DEB/MSI).
Preserve symlinks inside the macOS `.app` and executable permissions on Linux.
For CI transfers, create a `tar.gz` before storing the output as an artifact.

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

### Sharing one depot across multiple operating systems

Operating systems assigned the same ID in `steam.depots` form a group.
For example, assigning the same Depot ID to all operating systems separates their Steam installation paths as follows,
avoiding collisions between Electron files with identical names.

| OS | Example Steamworks Executable |
|---|---|
| Windows | `windows/my-game.exe` |
| macOS | `macos/My Game.app` |
| Linux | `linux/my-game` |

In Steamworks, configure the depot's OS setting for the shared operating systems or All OSes.
Use the Executable values from the generated `launch.json` for the Launch Options and set OS-specific launch conditions.
Every OS's files in the shared depot are distributed, so downloads are larger than with separate depots.
Valve recommends separate depots for OS-specific files, but does not prohibit sharing a depot across operating systems.
[Official depot OS settings](https://partner.steamgames.com/doc/store/application/depots)

Each OS export can succeed independently. Once every OS in a shared group has been exported, the builder automatically generates
`app_build.vdf`, `app_preview.vdf`, `depot_build.vdf` and `launch.json` in `dist/steam/shared/<env>/depot-<ID>/`.
If the group is incomplete, `launch.json` lists the missing operating systems and no upload VDFs are generated.
Standalone per-OS VDFs are not generated for a shared depot either.

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

The combined VDF uses `FileMapping` to place the OS packages under `windows/` / `macos/` / `linux/`.
Assets are not copied again, preserving the original packages' macOS symlinks and Linux executable permissions.
When moving the output, preserve the entire `dist/steam/` directory structure, not just the VDF files.
[Official FileMapping specification](https://partner.steamgames.com/doc/sdk/uploading)

For exports made on separate machines, such as in CI, collect each OS's `build/<env>/`, including `steam-package.json`
and `*-steampipe/`, under the same `dist/steam/<OS>/build/<env>/` structure, then run:

```sh
npx @next2d/builder --platform steam:macos --env prd --steam-manifest
```

The updated templates and `create-next2d-app` provide an npm script for the same operation:

```sh
npm run build:steam:manifest
# Combine packages from an environment other than prd
npm run build:steam:manifest -- --env dev
```

This command only regenerates shared depot VDFs; it does not build the web application or export Electron packages.
It can run on any OS. `--platform` supplies a Steam target for loading the Vite configuration; all shared depots in the configuration are combined.
Missing packages in a shared group cause exit code 1.
The command uses `build.outDir` and `--env` to select the most recent successful export for each OS, including its recorded `--arch`.
If x64 and arm64 are exported successively for the same OS, only the last successfully exported architecture is included.
It does not combine multiple CPU variants of the same OS into one shared depot.
`--steam-manifest` cannot be combined with `--arch` / `--preview` / `--build` / `--open`.
The App ID, Depot ID, game name, executable name and version must match the current configuration.
Even when retaining the same version number, make sure all distributed OS packages come from the intended revision.

Configurations where only some operating systems share an ID are supported. Other operating systems still get standalone VDFs.
Re-export the affected operating systems after changing the configuration or version. Old combined VDFs are invalidated during regeneration.

### Signing and notarizing macOS distribution builds

Steam uses the `darwin` target, not the Mac App Store target `mas`.
Valve requires 64-bit support and Apple notarization for new macOS applications.
The template entitlements include the library validation / DYLD settings needed for JIT and Steam Overlay,
but do not include `com.apple.security.app-sandbox`, which is separate from Chromium's renderer sandbox.
[Valve platform requirements](https://partner.steamgames.com/doc/store/application/platforms)

On macOS, add a Developer ID Application certificate to the keychain and save a notarization profile with
Apple's `xcrun notarytool store-credentials` before running the build. Do not store credentials in JSON.

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
NEXT2D_STEAM_RELEASE=1 npx @next2d/builder --platform steam:macos --env prd
```

`NEXT2D_STEAM_RELEASE=1` requires signing and notarization even when the JSON values are false.
Alternatively, set both `macos.sign` / `macos.notarize` to true in JSON.
Missing credentials, signing failures or notarization failures cause the build to fail.
Submit the package to SteamPipe after signing, notarization and stapling. Example checks:

```sh
codesign --verify --deep --strict 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
```

[Electron Packager signing and notarization options](https://electron.github.io/packager/main/interfaces/Options.html)

### Steamworks / SteamPipe

1. In Steamworks > SteamPipe > Depots, create depots matching your distribution layout and select their operating systems.
   Both separate OS depots and shared depots are supported. Use All languages for language-independent content.
   Include each depot in the development and retail Packages as well.
2. Add OS-specific Launch Options under Installation > General Installation.
   Set Executable to the generated `launch.json` value; leave Arguments/Working Directory empty.
   Select the 64-bit condition for x64 builds and test macOS Universal on both Intel and Apple Silicon.
3. Publish the changes in Steamworks. Match the supported operating systems and minimum OS requirements to
   the bundled Electron version and hardware testing, rather than relying only on older Steam minimum-OS tables.
4. Set the App ID/Depot IDs in JSON and rebuild. Standalone depot VDFs appear in `*-steampipe/`;
   shared depot VDFs appear in `shared/<env>/depot-<ID>/` once every required OS package exists.
   `app_preview.vdf` validates file mappings; `app_build.vdf` uploads the build.
   `depot_build.vdf` recursively maps the folder contents into the install root. `steam_appid.txt` is excluded from distribution.
5. Obtain ContentBuilder/SteamCMD from the Steamworks SDK and log in with an authorized build account.
   Do not store passwords in commands or the repository.

```text
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_preview.vdf" +quit
steamcmd +login BUILD_ACCOUNT +run_app_build "/absolute/path/to/app_build.vdf" +quit
```

ContentRoot is relative to the VDF. Move standalone applications together with their `*-steampipe/` directories;
for shared depots, preserve the entire `dist/steam/` layout.
VDFs produced by ordinary exports and `--steam-manifest` do not include SetLive.
After uploading, assign the build to a password-protected beta in Steamworks and test installation using the Steam client on each OS.
Each per-OS VDF updates one depot, so verify that the final build references the intended depot manifest versions for every OS.
[Official SteamPipe workflow and VDF specification](https://partner.steamgames.com/doc/sdk/uploading) /
[Depots](https://partner.steamgames.com/doc/store/application/depots)

### Uploading with the builder and testing on internal

`--steam-upload` uploads existing exported packages together through SteamCMD and uses `SetLive` to request activation on the selected beta branch.
It does not rebuild the web application or Electron packages.
`--platform` is unnecessary; do not combine this command with `--build` / `--preview` / `--open` / `--steam-manifest` / `--arch`.
This feature adds no npm dependencies to the builder. Install SteamCMD separately.

#### Preparing a private testing branch in Steamworks

1. Create an `internal` branch in Steamworks App Admin > SteamPipe > Builds.
2. **Set a branch password and share it only with the intended testers.** Naming a branch `internal` does not make it private.
   The builder does not create branches, set passwords or verify password protection. Do not store the password in JSON or the repository.
3. Make sure testers hold a Package license that grants access to the game and its depots.
   For external testers of an unreleased game, use Release State Override (beta) keys or another appropriate access method.
   A branch password alone does not grant ownership of the game.
4. In the Steam client, testers open the game's Properties > Game Versions & Betas, enter the password and select `internal`.

This command does not allow the `default` branch. Switch the public release build through Steamworks.
A beta password is a shared secret; another person who receives it can access the branch. Also manage each tester's game access rights.

[Official beta branch settings](https://partner.steamgames.com/doc/store/application/branches) /
[Testing on Steam](https://partner.steamgames.com/doc/store/testing) /
[Release State Override keys](https://partner.steamgames.com/doc/features/keys)

#### SteamCMD setup and authentication

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

#### Validation and upload

Collect the packages for every OS configured in `electron.config.json` under `steam.depots`, then run from the game root.
Operating systems sharing a Depot ID are placed in separate OS subdirectories; distinct depots are also uploaded in a single AppBuild.

```sh
# Local validation only: no SteamCMD, credentials, Steam connection or upload
npx @next2d/builder --steam-upload --env prd --dry-run

# Upload and request activation on steam.branch (internal by default)
npx @next2d/builder --steam-upload --env prd

# Specify a branch or the directory containing collected packages
npx @next2d/builder --steam-upload --env prd --steam-branch internal --steam-root dist/steam
```

During local development, replace `npx @next2d/builder` with `node ../builder/dist/index.js`.
After a builder version containing this feature is published, the game's `npm run upload:steam:check` / `npm run upload:steam` scripts can also be used.

`--steam-root` accepts a path relative to the game root or an absolute path; it defaults to `dist/steam`.
The upload operation does not read the Vite configuration, so specify this argument for a custom output directory.
It validates the App ID, Depot ID, version, game name and CPU recorded in `steam-package.json`, along with the executable.
Missing or inconsistent packages stop the operation before SteamCMD starts. VDFs are regenerated from validated packages instead of reusing existing files.

Each run saves its VDFs and `upload-plan.json` in `dist/steam/uploads/<env>/<branch>-<identifier>/`.
A dry run generates a VDF with `Preview=1` and no `SetLive`. This is local validation only;
it does not verify SteamCMD authentication, server-side permissions, branch existence or password settings.
Ordinary export VDFs are left unchanged. SteamPipe logs and incremental upload caches go in `uploads/<env>/cache/<branch>/`.

Uploads check the exit code and SteamCMD's success message, then record the BuildID in `upload-result.json`.
On the Steamworks Builds page, verify that the recorded BuildID is the current build for `internal`.
Steam may require additional confirmation depending on the account or app state.

### Pre-release validation

- Installation, launch and exit through Steam on each target OS, offline launch and save data after updates.
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
