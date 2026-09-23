# Xbox（試作・開発段階 / Prototype under development）

[日本語](#日本語) | [English](#english)

## 日本語

Xbox対応は現在、GDK向けネイティブホストを試作・検証している開発フェーズ。
ホスト生成やビルド処理は実装されているが、Xbox実機での動作確認・配布対応は完了していない。
現時点では、開発環境での検証を目的として使用する。

### 構成と現在の対応範囲

Next2DのJavaScriptをV8で実行し、Dawn（WebGPU → D3D12）で描画するC++ホストを生成する。
ElectronやWebViewは使用しない。

| 項目 | 現在の状態 |
|---|---|
| ホスト生成 | builder同梱のC++ / CMakeテンプレートから、ゲーム側の `xbox/` を生成・更新する。 |
| ゲーム設定 | ゲームルートの `MicrosoftGame.config` をホストに反映する。未配置の場合はテンプレートから初期ファイルを用意する。 |
| アセット | Webビルド結果とホストスクリプトを `assets.pak` / `assets.rc` にまとめ、実行ファイルへの埋め込み用に配置する。 |
| Windows上のビルド | CMakeでVisual Studio / GDK向けの構成・ビルドを実行する処理を実装している。対象環境での検証が必要。 |
| macOS / Linux上の実行 | ホスト生成とアセットの準備まで。GDK向け実行ファイルのビルドは行わない。 |
| Xbox実機・配布 | 開発機での動作、性能、保存処理、配布設定などの検証・調整が残っている。 |

### 検証に使う環境

以下は現行テンプレートが想定する構成。

| 項目 | 用途 |
|---|---|
| Windows + Visual Studio 2022 | C++ホストのビルド。現行builderはVisual Studio 2022ジェネレーターを指定する。 |
| Microsoft GDK | GDK向けビルド。PC向けの検証とXboxコンソール向けの環境を区別して用意する。 |
| CMake 3.26以降 | ホストの構成・ビルド。 |
| V8 | JavaScript実行エンジン。ビルド済みライブラリの取得、または自前ビルドを利用する。 |
| Dawn | WebGPU実装。CMakeのFetchContentで取得するため、初回取得にはネットワーク接続が必要。 |
| Xbox開発機（devkit） | Xbox実機上での動作・性能検証。 |

### 試作用コマンド

ゲームのルートで実行する。

```sh
npx @next2d/builder --platform xbox --env prd
```

Windowsではホストとアセットを準備した後、V8の解決とCMakeビルドへ進む。
macOS / Linuxではホスト生成とアセットの準備を終えた時点で終了する。
コマンドの終了だけでXbox実機向けの書き出しが完了したとは判断せず、生成物とログを確認する。

Windowsで `--open` または `--preview` を付けると、CMakeによる構成後にVisual Studioのソリューションを開く。
実行・デバッグはVisual Studio側で行う。

### V8の取得

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

### 検証範囲と残っている作業

`xbox-host-ci` ワークフローには、ラスタライザーの単体テスト、Windows APIを使う一部機能のテスト、
V8依存ソースのコンパイル確認、Game Core APIの確認を用意している。
これらはホスト全体のリンクやXbox実機での動作を保証するものではない。
Dawn / GDKを含む統合ビルドと、ゲームを使った実機検証が必要。

今後の検証・調整対象は、描画、Worker、入力、音声、テキスト、保存処理、性能、配布設定など。
詳細なホスト構成・自前V8のビルド・機能ごとの実装状況は、
[ホストテンプレートの開発資料](../templates/xbox/README.md)を参照する。
同じ資料は生成先の `xbox/README.md` にも配置される。

## English

Xbox support is currently in the development phase, focused on prototyping and testing a native GDK host.
Host generation and build operations are implemented, but validation on Xbox hardware and distribution support are not complete.
Use this feature for testing in a development environment at this stage.

### Architecture and current scope

The builder generates a C++ host that runs Next2D JavaScript on V8 and renders with Dawn (WebGPU → D3D12).
It does not use Electron or WebView.

| Item | Current status |
|---|---|
| Host generation | Creates or updates the game's `xbox/` directory from the C++ / CMake templates bundled with the builder. |
| Game configuration | Applies `MicrosoftGame.config` from the game root to the host. If it is missing, an initial file is created from the template. |
| Assets | Packs the web build output and host scripts into `assets.pak` / `assets.rc` for embedding into the executable. |
| Building on Windows | CMake configuration and build operations for Visual Studio / GDK are implemented. Validation in the target environment is still needed. |
| Running on macOS / Linux | Generates the host and prepares assets only. Does not build a GDK executable. |
| Xbox hardware and distribution | Device behavior, performance, save handling and distribution settings still need validation and adjustment. |

### Development environment

The current template targets the following setup.

| Component | Purpose |
|---|---|
| Windows + Visual Studio 2022 | Builds the C++ host. The current builder selects the Visual Studio 2022 generator. |
| Microsoft GDK | Builds the GDK target. Prepare the appropriate environment for PC testing or Xbox console development. |
| CMake 3.26 or later | Configures and builds the host. |
| V8 | JavaScript engine. Use the downloaded prebuilt library or your own build. |
| Dawn | WebGPU implementation. CMake FetchContent downloads it, so the first download needs network access. |
| Xbox development hardware (devkit) | Validates behavior and performance on Xbox hardware. |

### Prototype commands

Run from the game root:

```sh
npx @next2d/builder --platform xbox --env prd
```

On Windows, the builder prepares the host and assets, then resolves V8 and starts the CMake build.
On macOS / Linux, it stops after generating the host and preparing assets.
Command completion alone does not mean an Xbox hardware export is ready; inspect the output files and logs.

On Windows, `--open` or `--preview` opens the Visual Studio solution after CMake configuration.
Run and debug the application from Visual Studio.

### Acquiring V8

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

### Validation scope and remaining work

The `xbox-host-ci` workflow includes rasterizer unit tests, tests for selected Windows API functionality,
compilation checks for V8-dependent sources and Game Core API checks.
These checks do not guarantee that the complete host links or runs on Xbox hardware.
An integrated build with Dawn / GDK and testing with a game on the target hardware are still required.

Remaining validation and adjustments cover rendering, Workers, input, audio, text, save handling, performance and distribution settings.
For the detailed host architecture, instructions for building V8 yourself and implementation status by feature,
see the [host template development notes](../templates/xbox/README.md).
The same notes are copied to the generated `xbox/README.md`.
