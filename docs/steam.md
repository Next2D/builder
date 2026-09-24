# Steam 書き出し / アップロード / Export / Upload

[日本語](#日本語) | [English](#english)

## 日本語

ローカル書き出しはSTEP1・3・5を実施する。Steam配布時は全STEPが必要。

### STEP1：環境・アイコンを用意する

macOSのUniversal書き出し・署名・公証はmacOSで実行する。
テンプレートの `src/assets/icons/` の仮アイコンを、Windows用ICO・macOS用ICNS・Linux用PNGに差し替える。

| OS | 既定CPU | `architectures` / `--arch` の対応値 |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

### STEP2：Steamworksを設定する

1. 発行済みApp IDを確認する。
2. SteamPipe > DepotsでDepotを作成し、対象OS・言語を設定する。保存後、公開（Publish）タブで反映する。
3. 開発用・テスト用・販売用の該当Packageに必要なDepotを含める。
4. SteamPipe > Buildsで `internal` ブランチを作成し、パスワードを設定する。
5. テスターにゲーム・Depotの利用権とブランチのパスワードを渡す。未発売ゲームの外部テストにはRelease State Overrideキー等を使う。

**パスワードだけではゲームの利用権は付与されない。** builderはブランチ作成やパスワード設定を行わない。

| Depot構成 | 設定 |
|---|---|
| OS別（Valve推奨） | OSごとに異なる発行済みDepot IDを指定する。 |
| 複数OSで共用 | 同じIDを指定し、Steamworksの対象OSをAll OSesなどに合わせる。全対象OSのファイルを配布する。 |

Steamは複数Depotに対応。builderは各OSに1 IDまでで、一部OSだけの共用も可能。
同一OSを複数Depotへ分割する構成は未対応。ブランチごとにDepotを作る必要はない。

公式：[Depot](https://partner.steamgames.com/doc/store/application/depots) / [Package](https://partner.steamgames.com/doc/store/application/packages) / [テストと利用権](https://partner.steamgames.com/doc/store/testing)

### STEP3：electron.config.jsonを設定する

ゲームルートの設定を変更する。バージョンはルートの `package.json` を使う。

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

| 項目 | 設定・確認事項 |
|---|---|
| `appId` | bundle ID・保存先識別子。Steam App IDとは別。配布前に自分の値へ変更する。 |
| `appName` / `executableName` | アプリ表示名 / 実行ファイル名。 |
| `companyName` | Windowsの会社名。省略時は `package.json` の `author.name`、なければゲーム名。 |
| `description` | 任意。未指定・`null` は `package.json` の説明を使用。 |
| `icons` | ゲームルートからの相対パスまたは絶対パス。省略したOSは仮アイコン、指定ファイルがなければ失敗。 |
| `architectures` | STEP1のCPU設定。`--arch` が優先。 |
| `steam.appId` / `steam.depots` | 配布時は `null` を実際のApp ID・Depot IDへ変更する。Depot IDをApp IDから推測しない。未設定のOSはアップロード対象外。 |
| `steam.branch` | 反映先。既定は `internal`。 |
| `macos.sign` / `macos.notarize` | ローカル検証は `false`、配布時は両方 `true`。 |

### STEP4：認証を用意する

#### macOSの署名・公証

キーチェーンに **Developer ID Application証明書と秘密鍵** を登録し、署名IDを確認する。

```sh
security find-identity -v -p codesigning
```

表示された `Developer ID Application: ...` の名前全体を指定する。
公証プロファイルは `xcrun notarytool store-credentials` で事前登録した名前を使う。
Apple ID認証ではアプリ用パスワードが必要。認証情報はJSONへ保存しない。

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
```

署名・公証が失敗したらアップロードへ進まない。`Signature=adhoc` / `TeamIdentifier=not set` はDeveloper ID署名未完了。

#### SteamCMD

[Steamworks SDKのContentBuilder](https://partner.steamgames.com/doc/sdk/uploading)からSteamCMDを用意する。
ビルドアカウントには `Edit App Metadata` と `Publish App Changes To Steam` を付与する。
macOS / Linuxの例（パスとSteamログイン名を置き換える）:

```sh
export STEAMCMD="/absolute/path/to/steamcmd.sh"
export STEAM_USERNAME="your_build_account"
"$STEAMCMD" +login "$STEAM_USERNAME" +quit
```

- パスワード・要求されたSteam Guardコードを入力し、**ログイン後の正常終了まで待つ**。手動ログインなら `quit` で終了する。
- builderは同じSteamCMD・アカウントの保存済み認証を使う。`Cached credentials not found` が出たら上のログインコマンドを再実行する。
- Windowsは `steamcmd.exe` とPowerShellの `$env:STEAMCMD` / `$env:STEAM_USERNAME` を使用。`STEAMCMD` は実行ファイルのパスのみ、`.sh` は実行権限が必要。
- CIではSteamCMDの `config/config.vdf` をSecretとして復元し、成果物に含めない。

### STEP5：書き出し・成果物を確認する

ゲームルートで実行する。追加のアプリビルドやインストーラー作成は不要。

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

- CPU変更は `--arch arm64` 等を追加する。同一OSは最後に成功したCPUの成果物を使う。
- 出力は `dist/steam/<OS>/build/<env>/`。実行ファイルだけでなく、配下のフォルダ全体を保持する。
- 別マシン・CIから集める場合も `steam-package.json` と `*-steampipe/` を含む同じ階層を保つ。`tar.gz` で転送し、シンボリックリンク・実行権限を維持する。
- 設定・バージョン変更後は対象OSを再書き出しする。アップロード対象の全OSを同じバージョン・意図したリビジョンで揃える。

macOS配布用は次を実行する（パスは成果物に合わせる）:

```sh
codesign --verify --deep --strict --verbose=2 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
echo $?
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
echo $?
```

| 検証 | 正常時の出力 | 直後の `echo $?` |
|---|---|---|
| `codesign` | `valid on disk` / `satisfies its Designated Requirement` | `0` |
| `stapler` | `The validate action worked!` | `0` |

`0` 以外ならエラーを解消して再検証する。これらは検証コマンドで、署名・公証自体は実行しない。

### STEP6：起動設定・アップロード・動作確認

#### 起動ファイルを設定する

SteamworksのInstallation > General InstallationでOS別Launch Optionを設定する。
**同じDepot IDを複数OSで使う場合に、起動パスへOS名が付く。** 例はSTEP3のゲーム名・実行ファイル名に対応する。

| Depot構成 | WindowsのExecutable | macOSのExecutable | LinuxのExecutable |
|---|---|---|---|
| 単一Depot・単一OS（Windowsの例） | `my-game.exe` | — | — |
| 単一Depot・全OS共用 | `windows/my-game.exe` | `macos/My Game.app` | `linux/my-game` |
| 複数Depot・OS別 | `my-game.exe` | `My Game.app` | `my-game` |
| 複数Depot・Windows/macOSのみ共用 | `windows/my-game.exe` | `macos/My Game.app` | `my-game` |

- 単一OSがmacOS / Linuxの場合もOS名を付けず、`My Game.app` / `my-game` を指定する。
- インストール先からの相対パスを使い、`dist/steam/...` は含めない。実際の値は `*-steampipe/launch.json` またはdry-runで確認する。
- Arguments / Working Directoryは空欄。OS・CPU条件（x64は64bit）を設定し、公開（Publish）で反映する。Depot構成変更時は起動パスも更新する。

#### アップロードする

STEP4のログインが正常終了したターミナルで実行する。

```sh
npx @next2d/builder --steam-upload --env prd --dry-run
npx @next2d/builder --steam-upload --env prd
```

設定した全OSの成果物を検証し、全Depotを1回でアップロードする。事前の `--steam-manifest` は不要。
dry-runはローカル検証のみで、Steam側の権限・ブランチ・パスワードは確認しない。

| 任意の引数 | 用途 |
|---|---|
| `--steam-branch internal` | 反映先を上書き。`default` への切り替えはSteamworksで行う。 |
| `--steam-comment "操作修正"` | ビルド説明。未指定は `<appName> <version> (<env>, <branch>)`。空白のみ・制御文字・改行・ダブルクォート・バックスラッシュは不可。 |
| `--steam-root dist/steam` | 成果物の収集先。カスタム出力先では指定必須。 |

アップロードには `--platform` / `--arch` / `--build` / `--preview` / `--open` / `--steam-manifest` を併用しない。

#### 完了を確認する

1. `dist/steam/uploads/<env>/<branch>-<識別子>/upload-result.json` のBuildIDが、Steamworksの対象ブランチに反映されていることを確認する。カスタム出力先では `dist/steam` を読み替える。
2. Steamクライアントのプロパティ > ゲームバージョンとベータでパスワードを入力し、`internal` を選ぶ。
3. 各OSでインストール・起動・終了・入力・音声・全画面・オフライン起動・更新後の保存を確認する。macOS UniversalはIntel / Apple Silicon両方で確認する。
4. 対応を掲げるOverlay・Steam Deck・Linux環境を実機で確認し、ストア記載と一致させる。実績・DRM・Steam Cloudは自動統合されない。

エラー時のSteamPipeログは `dist/steam/uploads/<env>/cache/<branch>/` を確認する。

## English

For local exports, follow STEP1, STEP3 and STEP5. Steam distribution requires all steps.

### STEP1: Prepare the environment and icons

Run macOS Universal exports, signing and notarization on macOS.
Replace the template icons in `src/assets/icons/` with Windows ICO, macOS ICNS and Linux PNG files.

| OS | Default CPU | Accepted `architectures` / `--arch` values |
|---|---|---|
| Windows | `x64` | `x64`, `arm64` |
| macOS | `universal` | `x64`, `arm64`, `universal` |
| Linux | `x64` | `x64`, `arm64` |

### STEP2: Configure Steamworks

1. Find the issued App ID.
2. Create depots in SteamPipe > Depots and set their OS/language. Save, then apply the changes on the Publish tab.
3. Include the required depots in the relevant development, testing and retail Packages.
4. Create an `internal` branch in SteamPipe > Builds and set a password.
5. Give testers game/depot access and the branch password. Use Release State Override keys or equivalent access for external testing before release.

**A branch password does not grant game access.** The builder does not create branches or configure passwords.

| Depot layout | Configuration |
|---|---|
| Separate per OS (Valve recommendation) | Assign a different issued Depot ID to each OS. |
| Shared across operating systems | Assign the same ID and set the Steamworks OS selection accordingly, such as All OSes. All included OS files are distributed. |

Steam supports multiple depots. The builder accepts one ID per OS, including layouts where only some operating systems share a depot.
Splitting one OS across multiple depots is not supported. Separate depots per branch are unnecessary.

Official: [Depots](https://partner.steamgames.com/doc/store/application/depots) / [Packages](https://partner.steamgames.com/doc/store/application/packages) / [Testing and access](https://partner.steamgames.com/doc/store/testing)

### STEP3: Configure electron.config.json

Edit the configuration at the game root. The version comes from the root `package.json`.

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

| Field | Setup / checks |
|---|---|
| `appId` | Bundle ID and save location identifier, separate from the Steam App ID. Replace before distribution. |
| `appName` / `executableName` | Application display name / executable file name. |
| `companyName` | Windows company name. Falls back to `author.name` in `package.json`, then the game name. |
| `description` | Optional. Omitted or `null` uses the description from `package.json`. |
| `icons` | Paths relative to the game root, or absolute paths. Omitted OS entries use placeholders; missing specified files cause an error. |
| `architectures` | CPU settings from STEP1. `--arch` takes priority. |
| `steam.appId` / `steam.depots` | For distribution, replace `null` with actual App/Depot IDs. Do not infer Depot IDs from the App ID. Operating systems without a Depot ID are excluded from uploading. |
| `steam.branch` | Target branch; defaults to `internal`. |
| `macos.sign` / `macos.notarize` | Both `false` for local testing; both `true` for distribution. |

### STEP4: Prepare credentials

#### macOS signing and notarization

Install a **Developer ID Application certificate and private key** in the keychain, then find the signing identity:

```sh
security find-identity -v -p codesigning
```

Use the complete `Developer ID Application: ...` name shown.
Use a notarization profile previously registered with `xcrun notarytool store-credentials`.
Apple ID authentication requires an app-specific password. Do not store credentials in JSON.

```sh
export APPLE_SIGNING_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export APPLE_NOTARY_PROFILE='your-notary-profile'
```

Resolve signing/notarization failures before uploading. `Signature=adhoc` / `TeamIdentifier=not set` means Developer ID signing is incomplete.

#### SteamCMD

Obtain SteamCMD from the [Steamworks SDK ContentBuilder](https://partner.steamgames.com/doc/sdk/uploading).
Grant the build account `Edit App Metadata` and `Publish App Changes To Steam` permissions.
Example for macOS / Linux; replace the path and Steam login name:

```sh
export STEAMCMD="/absolute/path/to/steamcmd.sh"
export STEAM_USERNAME="your_build_account"
"$STEAMCMD" +login "$STEAM_USERNAME" +quit
```

- Enter the password and any requested Steam Guard code, then **wait for normal exit after login**. After manual login, enter `quit`.
- The builder reuses saved credentials for the same SteamCMD/account. If `Cached credentials not found` appears, rerun the login command above.
- On Windows, use `steamcmd.exe` and PowerShell's `$env:STEAMCMD` / `$env:STEAM_USERNAME`. `STEAMCMD` accepts only an executable path; `.sh` files need executable permission.
- In CI, restore SteamCMD's `config/config.vdf` from a Secret and exclude it from artifacts.

### STEP5: Export and check the packages

Run from the game root. No additional application build or installer is needed.

```sh
npx @next2d/builder --platform steam:windows --env prd
npx @next2d/builder --platform steam:macos --env prd
npx @next2d/builder --platform steam:linux --env prd
```

- Append `--arch arm64` or another supported value to change the CPU. Each OS uses its last successful architecture export.
- Output is under `dist/steam/<OS>/build/<env>/`. Keep the entire contents, not just the executable.
- When collecting packages from other machines or CI, preserve this layout, including `steam-package.json` and `*-steampipe/`. Transfer as `tar.gz` to retain symlinks and executable permissions.
- Re-export affected operating systems after configuration/version changes. Keep every upload target on the same version and intended revision.

For macOS distribution, run these checks with your package path:

```sh
codesign --verify --deep --strict --verbose=2 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
echo $?
xcrun stapler validate 'dist/steam/macos/build/prd/My Game-darwin-universal/My Game.app'
echo $?
```

| Check | Successful output | Immediately following `echo $?` |
|---|---|---|
| `codesign` | `valid on disk` / `satisfies its Designated Requirement` | `0` |
| `stapler` | `The validate action worked!` | `0` |

Resolve nonzero results and verify again. These commands check the package; they do not sign or notarize it.

### STEP6: Configure launching, upload and test

#### Configure executable paths

Set OS-specific Launch Options in Steamworks under Installation > General Installation.
**An OS prefix is added when multiple operating systems use the same Depot ID.** Examples use STEP3's application/executable names.

| Depot layout | Windows Executable | macOS Executable | Linux Executable |
|---|---|---|---|
| One depot, one OS (Windows example) | `my-game.exe` | — | — |
| One depot shared by all operating systems | `windows/my-game.exe` | `macos/My Game.app` | `linux/my-game` |
| Multiple depots, separate per OS | `my-game.exe` | `My Game.app` | `my-game` |
| Multiple depots, Windows/macOS sharing only | `windows/my-game.exe` | `macos/My Game.app` | `my-game` |

- A macOS-only / Linux-only depot also omits the OS prefix: use `My Game.app` / `my-game`.
- Paths are relative to the installation directory; omit `dist/steam/...`. Check actual values in `*-steampipe/launch.json` or dry-run output.
- Leave Arguments / Working Directory empty. Set OS/CPU conditions (64-bit for x64), then Publish. Update launch paths when changing depot layouts.

#### Upload

Use the terminal where STEP4's login exited successfully.

```sh
npx @next2d/builder --steam-upload --env prd --dry-run
npx @next2d/builder --steam-upload --env prd
```

The builder validates all configured OS packages and uploads all depots in one operation. No prior `--steam-manifest` is needed.
Dry-run checks local files only, not Steam permissions, branches or passwords.

| Optional argument | Purpose |
|---|---|
| `--steam-branch internal` | Overrides the target branch. Switch `default` through Steamworks. |
| `--steam-comment "Input fix"` | Build description. Defaults to `<appName> <version> (<env>, <branch>)`. Whitespace-only text, control characters, newlines, double quotes and backslashes are not allowed. |
| `--steam-root dist/steam` | Package collection root. Required for custom output locations. |

Do not combine uploads with `--platform` / `--arch` / `--build` / `--preview` / `--open` / `--steam-manifest`.

#### Verify completion

1. Check that the BuildID in `dist/steam/uploads/<env>/<branch>-<identifier>/upload-result.json` is active on the target Steamworks branch. Substitute your custom output root for `dist/steam` if applicable.
2. In the Steam client's Properties > Game Versions & Betas, enter the password and select `internal`.
3. Test installation, launch, exit, input, audio, fullscreen, offline launch and saves after updates on each OS. Test macOS Universal on both Intel and Apple Silicon.
4. Test advertised Overlay, Steam Deck and Linux support on hardware, matching the store listing. Achievements, DRM and Steam Cloud are not integrated automatically.

For failures, inspect SteamPipe logs in `dist/steam/uploads/<env>/cache/<branch>/`.
