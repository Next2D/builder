# 共通準備 / Common setup

[日本語](#日本語) | [English](#english)

## 日本語

各プラットフォームの手順に進む前に、ゲームのビルド環境を準備する。

### STEP1：Node.js・npm・npxを用意する

利用するbuilderとゲームの依存パッケージが要求するNode.jsをインストールし、次のコマンドを使える状態にする。

```sh
node --version
npm --version
npx --version
```

初回のツール取得にはネットワーク接続が必要。取得後はnpmキャッシュを再利用する。
builderはElectron PackagerやCapacitor SDKを必要な操作時に取得するが、
Xcode・Android SDK・GDKなどのネイティブ開発環境はユーザーが別途用意する。

### STEP2：ゲームの依存をインストールする

ゲームの `package.json` があるディレクトリで実行する。

```sh
npm ci
```

`package-lock.json` がない新規プロジェクトは `npm install` を使う。
以降の各ガイドのコマンドも、特記がなければゲームルートで実行する。
`@next2d/builder` は `npx` で起動できるため、ゲームの依存へ追加する必要はない。

### STEP3：対象プラットフォームの準備へ進む

| ガイド | ユーザーが用意するもの |
|---|---|
| [Electron / Steam](steam.md#日本語) | アイコン・対象OSの検証環境。Steam配布時はApp/Depot・ブランチ・SteamCMD、macOS配布時は署名・公証の認証。 |
| [iOS / Android](capacitor.md#日本語) | 対象OSのIDE・SDK、アプリ設定、配布時の署名。 |
| [Xbox（試作）](xbox.md#日本語) | WindowsのC++ / GDK開発環境。コンソール検証時は対応する開発環境・開発機。 |

### 補足：隣接リポジトリのbuilderを開発中に使う場合

builderリポジトリで `npm ci` を実行し、ソース変更後は `npm run build` で `dist/` を更新する。
ゲームルートでは各ガイドの `npx @next2d/builder` を `node ../builder/dist/index.js` に置き換える。
ゲーム側の `package.json` を変更せずに開発版を使える。

## English

Prepare the game build environment before following a platform guide.

### STEP1: Install Node.js, npm and npx

Install a Node.js version supported by the builder and the game's dependencies, and verify these commands work:

```sh
node --version
npm --version
npx --version
```

Initial tool downloads require network access; later runs reuse npm's cache.
The builder acquires Electron Packager and Capacitor SDKs when needed, but you must separately install
native development tools such as Xcode, the Android SDK or the GDK.

### STEP2: Install the game's dependencies

Run in the directory containing the game's `package.json`:

```sh
npm ci
```

For a new project without `package-lock.json`, use `npm install`.
Run subsequent guide commands from the game root unless stated otherwise.
`@next2d/builder` runs through `npx`, so it does not need to be added to the game's dependencies.

### STEP3: Continue with the target platform

| Guide | What you need to prepare |
|---|---|
| [Electron / Steam](steam.md#english) | Icons and environments for testing each OS. Steam distribution needs an app/depots, a branch and SteamCMD; macOS distribution needs signing/notarization credentials. |
| [iOS / Android](capacitor.md#english) | Platform IDEs/SDKs, app configuration and distribution signing. |
| [Xbox (prototype)](xbox.md#english) | A Windows C++ / GDK environment; console testing also needs the corresponding development environment and hardware. |

### Reference: Using a sibling builder repository during development

Run `npm ci` in the builder repository, then `npm run build` after source changes to update `dist/`.
From the game root, replace `npx @next2d/builder` in each guide with `node ../builder/dist/index.js`.
This uses the development build without changing the game's `package.json`.
