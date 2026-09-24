# Xbox（試作・開発段階 / Prototype under development）

[日本語](#日本語) | [English](#english)

## 日本語

Xbox対応はGDK向けネイティブホストの試作・開発段階。Xbox実機での動作確認・配布対応は完了していない。
以下は開発環境での検証手順。

### STEP1：検証範囲に必要な環境を用意する

| 検証範囲 | ユーザーが用意するもの |
|---|---|
| ホスト生成・アセット準備のみ | macOS / Linuxでも実行できる。 |
| C++ホストのビルド | Windows、Visual Studio 2022、Microsoft GDK、CMake 3.26以降。現行builderはVisual Studio 2022ジェネレーターを指定する。 |
| Xbox実機での検証 | コンソール向けGDK開発環境とXbox開発機（devkit）。PC向けのGDK検証環境とは区別して用意する。 |

### STEP2：ゲーム設定とV8を用意する

ゲームルートの `MicrosoftGame.config` にゲーム固有の値を設定する。
未配置の場合は初回のホスト生成でテンプレートから作られるため、生成後に内容を確認・変更する。

V8はJavaScript実行エンジン。通常はbuilderがビルド済みライブラリを取得し、自前ビルドを使う場合だけパスを指定する。
描画用のDawn（WebGPU → D3D12）はCMakeのFetchContentが取得するため、初回取得にはネットワーク接続が必要。

Windowsでのビルド時は、次の優先順でV8を解決する。

1. `--v8-root` で指定したパス
2. 環境変数 `V8_ROOT`
3. キャッシュ済みのビルド済みV8
4. Next2D/builderのGitHub Releasesからダウンロード

ビルド済みV8は `build-v8` ワークフローで作成・公開する構成になっている。
取得対象のバージョンとリビジョンはbuilder側で固定し、取得後はキャッシュを再利用する。
自前のV8を使用する場合:

```bat
npx @next2d/builder --platform xbox --env prd --v8-root C:\path\to\v8
```

公開しているWindows x64用V8の利用と、Xboxコンソール向けの互換性検証は別の作業。
コンソール向けにはGDKツールチェーンでの調整・再ビルドを含む検証が残っている。

### STEP3：ホストを生成・ビルドする

ゲームルートで実行する。

```sh
npx @next2d/builder --platform xbox --env prd
```

builder同梱のC++ / CMakeテンプレートから `xbox/` を生成・更新し、`MicrosoftGame.config` を反映する。
Webビルド結果とホストスクリプトを `assets.pak` / `assets.rc` にまとめ、実行ファイルへの埋め込み用に配置する。
このホストはV8とDawnを使い、ElectronやWebViewは使用しない。

| 実行環境・引数 | コマンドの到達点 |
|---|---|
| Windows | V8を解決し、CMakeでGDK向けの構成・ビルドを実行する。対象環境での検証が必要。 |
| Windows + `--open` / `--preview` | CMakeで構成した後、Visual Studioのソリューションを開く。実行・デバッグはVisual Studio側で行う。 |
| macOS / Linux | ホスト生成とアセット準備までで終了する。GDK向け実行ファイルは生成しない。 |

### STEP4：生成物と対象環境での動作を確認する

コマンドの終了だけでXbox向けの配布物が完成したとは判断せず、生成物とログを確認する。

`xbox-host-ci` ワークフローには、ラスタライザーとstbの回帰テスト、Windows APIを使う一部機能のテスト、
V8依存ソースのコンパイル確認、Game Core APIの確認を用意している。
これらはホスト全体のリンクやXbox実機での動作を保証するものではない。
Dawn / GDKを含む統合ビルドと、ゲームを使った実機検証が必要。

今後の検証・調整対象は、描画、Worker、入力、音声、テキスト、保存処理、性能、配布設定など。
詳細なホスト構成・自前V8のビルド・機能ごとの実装状況は、
[ホストテンプレートの開発資料](../templates/xbox/README.md)を参照する。
同じ資料は生成先の `xbox/README.md` にも配置される。

## English

Xbox support is a prototype GDK native host under development. Xbox hardware validation and distribution support are not complete.
The following steps are for development testing.

### STEP1: Prepare the environment for your validation scope

| Validation scope | What you need to prepare |
|---|---|
| Host generation and asset preparation only | Also works on macOS / Linux. |
| Building the C++ host | Windows, Visual Studio 2022, Microsoft GDK and CMake 3.26 or later. The current builder selects the Visual Studio 2022 generator. |
| Testing on Xbox hardware | A console GDK development environment and Xbox development hardware (devkit), distinct from a PC GDK testing environment. |

### STEP2: Prepare game configuration and V8

Set game-specific values in `MicrosoftGame.config` at the game root.
If absent, it is created from the template during initial host generation; review and edit it afterward.

V8 is the JavaScript engine. The builder normally downloads a prebuilt library; specify a path only when using your own build.
CMake FetchContent acquires Dawn (WebGPU → D3D12) for rendering, so the first download requires network access.

For Windows builds, V8 is resolved in this order:

1. The path passed with `--v8-root`
2. The `V8_ROOT` environment variable
3. A cached prebuilt V8
4. A download from the Next2D/builder GitHub Releases

The `build-v8` workflow is configured to build and publish prebuilt V8 libraries.
The builder pins the download version and revision and reuses the cache after downloading.
To use your own V8 build:

```bat
npx @next2d/builder --platform xbox --env prd --v8-root C:\path\to\v8
```

Using the published Windows x64 V8 library and validating Xbox console compatibility are separate tasks.
Console support still needs validation, including adjustments or rebuilds with the GDK toolchain.

### STEP3: Generate and build the host

Run from the game root:

```sh
npx @next2d/builder --platform xbox --env prd
```

The builder creates or updates `xbox/` from its bundled C++ / CMake templates and applies `MicrosoftGame.config`.
It packs the web build output and host scripts into `assets.pak` / `assets.rc` for embedding in the executable.
The host uses V8 and Dawn, without Electron or WebView.

| Environment / options | What the command completes |
|---|---|
| Windows | Resolves V8, then configures and builds the GDK target with CMake. Target-environment validation is still needed. |
| Windows + `--open` / `--preview` | Opens the Visual Studio solution after CMake configuration. Run and debug from Visual Studio. |
| macOS / Linux | Stops after host generation and asset preparation; no GDK executable is built. |

### STEP4: Check the output and target-environment behavior

Command completion alone does not mean an Xbox distribution package is ready; inspect the output and logs.

The `xbox-host-ci` workflow includes rasterizer and stb regression tests, tests for selected Windows API functionality,
compilation checks for V8-dependent sources and Game Core API checks.
These checks do not guarantee that the complete host links or runs on Xbox hardware.
An integrated build with Dawn / GDK and testing with a game on the target hardware are still required.

Remaining validation and adjustments cover rendering, Workers, input, audio, text, save handling, performance and distribution settings.
For the detailed host architecture, instructions for building V8 yourself and implementation status by feature,
see the [host template development notes](../templates/xbox/README.md).
The same notes are copied to the generated `xbox/README.md`.
