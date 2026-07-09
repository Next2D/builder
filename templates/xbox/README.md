Next2D Xbox Host (GDK native / V8 + Dawn WebGPU)
================================================

このディレクトリは `@next2d/builder --platform xbox` が生成する **Xbox GDK ネイティブタイトル** の
ホストプロジェクトです。Next2D の JavaScript を **V8** で実行し、描画を **Dawn(WebGPU → D3D12)** で行う
C++ 実行ファイルをビルドします。Electron や WebView は使用しません。

> **重要 / 検証ステータス**
> 本テンプレートは *アーキテクチャ的に整合したフルスキャフォールド* です。Xbox GDK タイトルの
> コンパイル・実機動作確認には **Windows + Visual Studio 2022 + Microsoft GDK + 開発機(devkit)** が
> 必須で、macOS/Linux では検証できません。GDK/実機が揃った段階で、下記「実機検証で残る作業」に沿って
> 最終調整(WebGPU バインディングの Next2D レンダラ実使用箇所への追従、画像アップロード経路、
> ストア設定値)を行ってください。

---

## アーキテクチャ

```
┌──────────────────────────────────────────────────────────────┐
│ Next2D アプリ (assets/app/*.js  ← builder が vite build で生成)  │
│   WebGPU / Canvas / rAF / fetch / Gamepad / WebAudio を使用      │
└───────────────▲──────────────────────────────────────────────┘
                │ グローバル(navigator.gpu, requestAnimationFrame, ...)
┌───────────────┴──────────────────────────────────────────────┐
│ js/bootstrap.js  TextEncoder/TextDecoder/URL 等の JS 層補完      │
└───────────────▲──────────────────────────────────────────────┘
                │ V8 バインディング (src/bindings/*)
┌───────────────┴──────────────────────────────────────────────┐
│ C++ ホスト (src/)                                               │
│  ├─ v8/V8Runtime          Isolate/Context/ESM/マイクロタスク     │
│  ├─ EventLoop             setTimeout/rAF/performance.now         │
│  ├─ bindings/             console/timers/fetch/Image/Audio/      │
│  │                        WebGPU(navigator.gpu)/Gamepad/DOM      │
│  ├─ gpu/DawnContext       Instance/Adapter/Device/Surface(D3D12) │
│  ├─ platform/GamepadManager  GameInput → W3C Gamepad             │
│  ├─ platform/AudioEngine     XAudio2 + Media Foundation デコード │
│  └─ main.cpp              GDK 初期化 / ウィンドウ / ゲームループ  │
└──────────────────────────────────────────────────────────────┘
```

ゲームループ(`main.cpp`)は毎フレーム:
1. Win32 メッセージ処理
2. ゲームパッド入力ポーリング(GameInput)
3. メインの タイマー実行 → マイクロタスク
4. メインの `requestAnimationFrame`（アプリのロジック/描画コマンド生成）
5. **Worker のメッセージ配送 + rAF**（レンダラ worker がここで WebGPU コマンドを submit）
6. V8 プラットフォームタスク + Dawn `ProcessEvents`
7. サーフェス Present

### ランタイム環境の要点

- **jitless V8**: Xbox/Switch のリテールは JIT(動的コード生成)を禁止するため、V8 は
  `--jitless --wasm-jitless` で起動する (JS=Ignition インタプリタ、wasm=DrumBrake インタープリタ)。
- **Worker / OffscreenCanvas**: player はレンダラを Worker + `transferControlToOffscreen`
  で動かす。本ホストは **「同一 Isolate 内の別 Context を協調スケジューリングする単一スレッド
  Worker モデル」** を採用（`src/worker/WorkerRuntime`）。`postMessage` は V8 の
  ValueSerializer による structured clone、OffscreenCanvas は transfer 対応。GPU を 1 スレッドに
  保てるため Dawn のスレッド安全性問題を避けられる。真のマルチスレッド化は将来の最適化。
- **Canvas 2D**: `src/bindings/Canvas2D.cpp` にソフトウェア実装。パス/変換/`isPointInPath`
  (ヒット判定の中核)・`fill`/`stroke`・`getImageData` を実装。実グリフ描画(`fillText` の見た目)は
  未実装（`measureText` は近似メトリクス）。
- **グラフィックスバックエンド抽象化**: `HostContext::backend`(WebGPU/WebGL2)で切替。Xbox は
  WebGPU。`navigator.gpu` はバックエンドが WebGPU のときのみ公開し、WebGL2(Switch)では
  player が `canvas.getContext('webgl2')` へ自動フォールバックする。canvas の getContext は
  `src/bindings/Canvas.cpp` が backend に応じて `2d`/`webgpu`/`webgl2` をディスパッチ。

---

## 必要環境

| 項目 | 内容 |
|------|------|
| OS | Windows 11 |
| IDE | Visual Studio 2022 (v143, C++ Game development workload) |
| GDK | 公開版 GDK をインストール (`-A Gaming.Desktop.x64`)、**または** NuGet `Microsoft.GDK.Windows` を展開して `-A x64` + 環境変数/`-D` `GDK_ROOT=<pkg>/native/<edition>/windows`(インストーラ不要・CI 向け)。コンソール向け GDKX は ID@Xbox 登録が必要 |
| CMake | 3.26 以降 |
| V8 | 不要 (builder が prebuilt を自動ダウンロード)。自前ビルドは `--v8-root` で指定可 |
| Dawn | CMake の FetchContent で自動取得(ネットワーク必須) |
| devkit | Xbox 開発機(実機検証用) |

---

## V8 の用意

**通常は何も必要ありません。** builder が Next2D の GitHub Releases から
prebuilt V8 monolith を自動ダウンロードし、`%LOCALAPPDATA%\next2d\v8\<version>` に
キャッシュします (マシンごとに初回のみ、以後は再利用)。

自動ダウンロードを使わない場合の優先順:

1. `--v8-root <path>` 引数
2. 環境変数 `V8_ROOT`
3. キャッシュ済み prebuilt
4. GitHub Releases から自動ダウンロード

> prebuilt は builder リポジトリの `build-v8.yml` ワークフロー (windows-latest) で
> ビルドして Releases に発行しています。バージョンは builder の `XBOX_V8_VERSION` に固定。

### 自前ビルドする場合 (任意)

コンソール実機向け (GDK ツールチェーン) や V8 バージョンを変えたい場合のみ。
**マシンごとに 1 回だけ**の作業で、以後は `--v8-root` で使い回せます。

事前準備 (V8 ビルド用):

| 項目 | 内容 |
|------|------|
| ディスク | 約 40GB (ソース + ビルド生成物) |
| 時間 | 初回 fetch 30〜60 分 + ビルド 1〜3 時間 |
| Git | インストール済みであること |
| depot_tools | [取得手順](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up)に従い展開し、**PATH の先頭**に追加 |
| 環境変数 | `DEPOT_TOOLS_WIN_TOOLCHAIN=0` (Google 社内ツールチェーンを使わない指定。未設定だと fetch が失敗する) |
| Windows SDK | Visual Studio Installer で「Windows 11 SDK」と「Windows 用デバッグツール」を含めること (GN が要求する) |

ビルド手順:

```bat
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
fetch v8
cd v8
:: CI のコンパイル検証と同じバージョンに固定 (xbox-host-ci.yml の V8_TAG)
git checkout 13.7.152.19
gclient sync
gn gen out\x64.release --args="v8_monolithic=true v8_use_external_startup_data=false is_component_build=false use_custom_libcxx=false icu_use_data_file=false treat_warnings_as_errors=false v8_enable_drumbrake=true target_cpu=\"x64\" is_debug=false v8_enable_pointer_compression=true v8_enable_sandbox=false"
ninja -C out\x64.release v8_monolith
```

生成物を次の構成で配置し、builder の `--v8-root` 引数 (または環境変数 `V8_ROOT`) で指すこと:

```
<V8_ROOT>/include/v8.h ...      (v8/include を丸ごとコピー)
<V8_ROOT>/lib/v8_monolith.lib   (v8/out/x64.release/obj/v8_monolith.lib)
```

> - `use_custom_libcxx=false` は必須。V8 は clang でビルドされるため、これを外すと
>   V8 同梱の libc++ が使われ、MSVC (MS STL) でビルドする本ホストとリンクできません。
> - `v8_enable_sandbox=false` は必須。sandbox は libc++ ハードニング(=V8 同梱 libc++)を
>   要求するため `use_custom_libcxx=false` と両立しません (BUILD.gn の assert で停止)。
>   本ホストは自明に信頼済みのゲーム JS のみ実行するため無効で問題ありません。
> - `icu_use_data_file=false` を推奨。外すと実行時に `icudtl.dat` の同梱が必要になります。
> - `treat_warnings_as_errors=false` を推奨。`use_custom_libcxx=false` では MSVC STL の
>   ヘッダ警告が -Werror で停止することがあります。**VS のバージョンは VS2022 (v143) を推奨**
>   (新しすぎる STL は V8 の clang が警告/エラーを出しやすい)。
> - `v8_enable_drumbrake=true` は必須 (r2)。**DrumBrake = V8 の wasm インタープリタ**で、
>   JIT 禁止環境 (コンソール) でも wasm を解釈実行できます。wasm はビルド時に turbofan を
>   要求するため `v8_jitless` ビルドは使えません — JIT の無効化はホストが実行時フラグ
>   `--jitless --wasm-jitless` で行います (JS=Ignition / wasm=DrumBrake)。
>   注意: DrumBrake は既定ビルド対象外のためタグによりコンパイル不能な場合があります
>   (13.6.233.17 は不可 → 13.7.152.19 で修正済み)。
> - pointer compression は `CMakeLists.txt` の `V8_COMPRESS_POINTERS` と一致させてください
>   (sandbox 無効に合わせ `V8_ENABLE_SANDBOX` は定義しません)。不一致は ABI 崩れの原因になります。
> - 上記は PC (`Gaming.Desktop.x64`) 用。コンソール実機 (`Gaming.Xbox.*`) 向けは
>   GDK ツールチェーンでの再ビルドが必要です (devkit 入手後の作業)。

---

## ビルド手順

builder 経由(推奨):

```bat
:: プロジェクトルートで。V8 は初回に自動ダウンロードされる (指定不要)
npx @next2d/builder --platform xbox --env prd
```

自前の V8 を使う場合のみ `--v8-root` を指定:

```bat
npx @next2d/builder --platform xbox --env prd --v8-root C:\path\to\v8
```

builder は次を行います:
1. Web 資材を `vite build` で生成
2. `xbox/assets/app` へ資材を配置
3. CMake で GDK 構成(`-A Gaming.Xbox.Scarlett.x64`)を生成
4. `cmake --build`（Release）でパッケージ生成

対象世代を変える場合は環境変数 `NEXT2D_XBOX_ARCH` を設定:
`Gaming.Xbox.Scarlett.x64`(既定) / `Gaming.Xbox.XboxOne.x64` / `Gaming.Desktop.x64`

Visual Studio で開いて実行/デバッグ:

```bat
npx @next2d/builder --platform xbox --env prd --open
```

CMake を直接叩く場合:

```bat
cmake -S xbox -B out/xbox/build -G "Visual Studio 17 2022" -A Gaming.Xbox.Scarlett.x64 -D V8_ROOT=%V8_ROOT%
cmake --build out/xbox/build --config Release
```

---

## テスト

Windows / GDK が無い環境でも検証できるよう、テストは 3 層に分かれています。

> 1・2 は **builder リポジトリ (Next2D/builder) 側**の開発用テストで、ゲーム側の
> `xbox/` にはコピーされません (`tests/` はスキャフォールド対象外)。
> ゲーム側で実行するのは 3 のセルフテストのみです。

### 1. どこでも実行できる単体テスト (`tests/raster_test.cpp`)

Canvas2D のソフトラスタライザ純ロジック (`src/bindings/RasterCore.h` —
塗り/winding/クリップ/色解析/曲線フラット化) は V8/Windows 非依存で、
macOS / Linux でそのまま実行できます:

```bash
# raster テスト + ASan + V8 ヘッダに対する構文チェックを一括実行
tests/check_local.sh
```

構文チェックは実 V8 ヘッダ (pin: `13.7.152.19`) に対して、Windows API 非依存の
V8 バインディング 12 ファイルをコンパイル検証します (API 削除・型不整合を検出)。

### 2. GitHub Actions (windows-latest) — `.github/workflows/xbox-host-ci.yml`

ローカルに Windows が無くても、push すると CI が Windows 上で実行します:

- **RasterCore 単体テスト** を MSVC でビルド・実行 (Linux では gcc + ASan/UBSan でも)
- **Windows 実 API smoke テスト** (`tests/windows_platform_test.cpp`):
  WIC の PNG デコード、DirectWrite/Direct2D のグリフラスタライズ (英/日)、
  Media Foundation の音声デコード、XAudio2 初期化 — を実際に実行して検証
- **V8 依存ソースのコンパイル検証** (V8 ヘッダ + MSVC、リンクなし)

CI 対象外 (実機セットアップ時に検証): Dawn 依存の `WebGPU.cpp` / `DawnContext.cpp` /
`main.cpp` (webgpu_cpp.h が Dawn ビルドで生成されるため) と GDK 依存の `GamepadManager.cpp`。

### 3. 実機セルフテスト (`js/selftest.js`)

ビルドが通ったら最初に実行するテスト。アプリの代わりに全バインディングを
実機/PC 上で網羅実行し、TAP 形式で結果を出して自動終了します:

```bat
Next2DXboxHost.exe --selftest
```

検証内容: タイマー/rAF/イベント、WIC 画像デコード、Canvas2D (fill/clip/
isPointInPath/fillText/drawImage)、Blob/URL/XHR、Worker (blob URL +
structured clone + OffscreenCanvas 転送)、**WebGPU 実行経路** (バッファ読み戻し、
copyExternalImageToTexture、**dynamic offset 付き描画**、**stencil8 マスク描画**)、
音声 (decodeAudioData/gain/ended)、ゲームパッド、クリップボード。
終了コード 0 = 全パス。

---

## WebGPU バインディングの実装範囲

`src/bindings/webgpu/` は **Next2D の WebGPU レンダラが使用するコア経路** を実装しています:

- `navigator.gpu` : `requestAdapter` / `getPreferredCanvasFormat`
- `GPUAdapter` : `requestDevice`
- `GPUDevice` : `createBuffer` / `createTexture` / `createSampler` / `createShaderModule` /
  `createBindGroupLayout` / `createBindGroup` / `createPipelineLayout` /
  `createRenderPipeline` / `createCommandEncoder` / `queue` / `limits`
- `GPUQueue` : `submit` / `writeBuffer` / `writeTexture` / `copyExternalImageToTexture`
  (画像/テキスト/動画のテクスチャ化。CPUで flipY/premultiply を適用し WriteTexture へ)
- `GPUCommandEncoder` : `beginRenderPass` / `finish` / `copyBufferToBuffer` /
  `copyTextureToTexture` / `copyTextureToBuffer`(ピクセル読み戻し)
- `GPURenderPassEncoder` : `setPipeline` / `setBindGroup`(**dynamic offsets 対応**) /
  `setVertexBuffer` / `setIndexBuffer` / `setViewport` / `setScissorRect` /
  `setStencilReference`(**マスク処理**) / `setBlendConstant` / `draw` / `drawIndexed` /
  `drawIndirect`(コンテナ一括描画) / `end`
- ステンシル: `depthStencil`(`stencilFront`/`stencilBack`/`stencilReadMask`/`stencilWriteMask`、
  `stencil8` フォーマット)と `depthStencilAttachment`(`stencilLoadOp`/`stencilStoreOp`/
  `stencilClearValue`)を解析。Next2D のマスク/クリップ描画の中核。
- `GPUBuffer` : `getMappedRange` / `mapAsync`(READ) / `unmap` / `destroy`
- `GPUTexture` : `createView` / `destroy`
- `canvas.getContext('webgpu')` : `configure` / `getCurrentTexture`

> player(`@next2d/player`) の実 API 使用を突合済み。**コンピュートシェーダ / render bundle /
> indexed draw / importExternalTexture は player が未使用**のため未実装で問題なし。
- 定数: `GPUBufferUsage` / `GPUTextureUsage` / `GPUShaderStage` / `GPUColorWrite` / `GPUMapMode`

WebGPU IDL は約 40 型・数百メソッドあり、全面手書きは現実的でないため、上記コアを実装し
拡張ポイントを `«EXTEND»` コメントで明示しています。追加が必要になった API は同じラップ
パターン(`WrapWith` / `Unwrap` / ディスクリプタ解析ヘルパー)で追記してください。

---

## Next2D 機能カバレッジ (player 突合)

player の実 API 使用を全面調査し、以下を実装済み。**この環境(macOS/GDK・devkit 無)では
コンパイル・実機検証は不可**のため、下表は「コード上の対応状況」であり実機での見た目・挙動は
別途検証が必要です。

| 機能 | 状態 | 備考 |
|------|------|------|
| WebGPU コア描画 (shape/mask/blend/pipeline) | ✅ 実装 | 初期化 limits も対応 |
| マスク/クリップ (ステンシル + `setStencilReference` + dynamic offset) | ✅ 実装 | `stencil8`/`stencilFront`/`stencilBack` を解析 |
| コンテナ一括描画 (`drawIndirect`) | ✅ 実装 | |
| 画像/テキスト/動画のテクスチャ化 (`copyExternalImageToTexture`) | ✅ 実装 | flipY/premultiply を CPU 適用 |
| フィルタ (`copyTextureToTexture` + fragment shader) | ✅ 実装 | コンピュート不使用のため対応可 |
| ピクセル読み戻し (`copyTextureToBuffer`+`mapAsync`) | ✅ 実装 | BitmapData/getPixels |
| ヒット判定 (Canvas2D `isPointInPath`) | ✅ 実装 | ソフトラスタ |
| **テキスト表示** (`fillText`/`measureText`/`clip`) | ⚠️ 実装(要検証) | DirectWrite/Direct2D。`clip` は矩形近似(TextField 枠に十分)。ベースライン/合字/絵文字は実機検証 |
| アセット読込 (`XMLHttpRequest`) | ✅ 実装 | assets/blob から同期読込 |
| Worker/OffscreenCanvas (レンダラ) | ✅ 実装(要検証) | 協調シングルスレッド + `?worker&inline`(blob) 対応 |
| `Blob`/`URL.createObjectURL`/`ImageData` | ✅ 実装 | |
| ゲームパッド入力 | ✅ 実装 | `navigator.getGamepads` ポーリング |
| サウンド (WebAudio 最小) | ⚠️ 実装(要検証) | decode/play/stop/音量/ミュート/`ended` 発火に対応。AnalyserNode 等は未対応 |
| ポインタ/キーボード/ホイール入力 | ⚠️ 実装(要検証) | `WndProc`→`DispatchEvent`。配送先canvasの特定は実機検証 |
| 動画 (HTMLVideoElement) | ⚠️ 実装(要検証) | `Video.cpp`(Media Foundation)。フレーム精度/音声同期は実機検証 |
| クリップボード (`navigator.clipboard`) | ✅ 実装 | Win32 クリップボード (readText/writeText) |
| **JS エンジン/グローバル機能** (player 全数調査) | ✅ 実装 | Proxy=V8コア。`crypto.randomUUID`/`getRandomValues`(BCrypt)、`TextEncoder`/`TextDecoder`/`queueMicrotask`/`location`/`atob`/`btoa` は worker 含む全コンテキストへ注入。`createImageBitmap` は ImageData/ImageBitmap/canvas/Blob を受理 |
| **WebAssembly** | ✅ 実装 (要 V8 r2) | prebuilt V8 r2 = wasm + **DrumBrake**(V8 の wasm インタープリタ、JIT 禁止コンソール向け)。実行時 `--jitless --wasm-jitless` で JS=Ignition / wasm=DrumBrake 解釈実行 |
| **indexedDB** (最小実装) | ✅ 実装 | open/createObjectStore/transaction/get/put/delete/clear/getAll/count/keyPath。値は JSON 化可能なもの + ArrayBuffer/TypedArray(base64 永続化)。カーソル/インデックスは未対応。保存先 `%LOCALAPPDATA%\Next2D\idb_<db>.json` |
| **localStorage / sessionStorage** | ✅ 実装 | JS セマンティクス + ファイル永続化 (`%LOCALAPPDATA%\Next2D\`)。コンソール実機では XGameSave 化が必要 («EXTEND») |
| **Audio 要素** (`new Audio(url)`) | ✅ 実装 | play/pause/paused/loop/volume/currentTime/preload (pause→play は先頭から) |

> player が使用する API はコード上すべて実装済み。残る唯一の障壁は
> **GDK/devkit を要するコンパイル・実機検証**(この環境では物理的に不可)。

## 実機検証で残る作業

0. **まずセルフテスト**: ビルドが通ったら `Next2DXboxHost.exe --selftest` を実行。
   全バインディングを一括検証し、失敗箇所が TAP 形式で列挙されます (上記「テスト」参照)。
1. **Dawn / V8 API 追従**: `FetchDawn.cmake` の `DAWN_TAG` と V8 バージョンで
   webgpu_cpp のシンボル名(例 `ShaderSourceWGSL` / `Limits` / `GetLimits` の戻り値型)が
   異なる場合があります。該当タグの `webgpu_cpp.h` に合わせて調整してください。
2. **テキストの見た目**: `Canvas2D.cpp` の `fillText`/`measureText` は DirectWrite/Direct2D で
   実装済みですが、ベースライン位置・`textAlign`/`textBaseline`・合字/絵文字は実機で要検証。
3. **ポインタ/キーボード入力**: `main.cpp` の `WndProc` でマウス/キー/ホイールを
   `DispatchEvent` 経由で window/document/主要canvas へ配送済み。ただし player がリスナを
   登録する canvas インスタンスの特定はヒューリスティック(最後に生成した canvas)のため、
   実機で正しい配送先か検証・調整してください。キーボードは window 登録で信頼できます。
4. **動画 (任意機能)**: `<video>`(HTMLVideoElement) は `Video.cpp`(Media Foundation)で
   デコード/フレーム供給を実装済み。フレーム精度・音声同期は実機で要検証。
5. **サウンド**: source→gain→destination の最小グラフ・音量・ミュート・`ended` 発火を
   実装済み(`Audio.cpp`)。再生タイミング/レイテンシは実機で要検証。AnalyserNode 等の
   高度なノードは未対応(player 未使用)。
6. **Worker スクリプト形式**: `WorkerRuntime` はクラシック/blob worker を評価します。
   ESM worker の場合は IIFE 出力設定にするか `WorkerInstance::Start` を module 評価へ拡張。
7. **GameInput 複数台 / パフォーマンス(GC・present モード)**: 実機で調整。
7. **MicrosoftGame.config**: **プロジェクトルート**で管理(capacitor.config.json と同じ)。
   `TitleId`/`StoreId`/`MSAAppId` を Partner Center 値へ置換、ストアロゴ(`xbox/assets/*.png`)を用意。
8. **jitless の性能**: JIT 無効化のオーバーヘッドは V8 スナップショット等で緩和を検討。

## Nintendo Switch への展開

本ホストは Switch 移植を見据えた構成です。共通コア(V8/Worker/入力/音声/Canvas2D/AssetLoader)は
そのまま流用でき、差分は主に以下:

- **JIT 禁止は Switch も同様** → jitless V8 の構成をそのまま使用。
- **グラフィックス**: Dawn に Switch(NVN) バックエンドは無いため、`HostContext::backend` を
  `WebGL2` にして `canvas.getContext('webgl2')` を Switch の GL に実装する
  (`Canvas.cpp` の `«SWITCH»`)。player は WebGL2 バックエンドを持つため描画コードは再利用できる。
- **SDK**: NintendoSDK は NDA 下で、承認 + devkit が必須。SDK 呼び出し箇所を埋めた
  スキャフォールドとして拡張する。

---

## ディレクトリ構成

```
xbox/
├─ CMakeLists.txt              GDK ビルド構成
├─ MicrosoftGame.config        GDK ゲーム設定 (要 ID@Xbox 値)
├─ cmake/
│  ├─ FetchDawn.cmake          Dawn 取得(D3D12 バックエンド)
│  └─ FindV8.cmake             prebuilt V8 の探索
├─ js/bootstrap.js             JS 層のブラウザ相当補完
├─ assets/app/                 builder が Web 資材を配置
└─ src/
   ├─ main.cpp
   ├─ HostContext.h
   ├─ AssetLoader.{h,cpp}
   ├─ EventLoop.{h,cpp}
   ├─ v8/{V8Runtime,V8Util}.*
   ├─ gpu/DawnContext.{h,cpp}
   ├─ platform/{GamepadManager,AudioEngine}.*
   └─ bindings/
      ├─ Bindings.h
      ├─ Console.cpp Timers.cpp Fetch.cpp Image.cpp Audio.cpp
      ├─ Gamepad.cpp DomShims.cpp
      └─ webgpu/{WebGPU.cpp,WebGPUCommon.h,WebGPUEnums.h}
```

---

## コンソール (Game Core OS) とテキスト描画・フォント

コンソールには DirectWrite / システムフォントが存在しないため、`fillText`/`measureText`
は同梱の stb_truetype 実装 (`platform/StbTextRasterizer`) が担う。フォントは実行時登録:

- **ゲーム資材に TTF/OTF を含める**(推奨): Web ビルドの資材 (`assets/app`) に
  `*.ttf` / `*.otf` があれば起動時に自動登録される (vite の `public/` に置く等)。
  日本語テキストを使う場合は Noto Sans JP (OFL) などの同梱を推奨。
- 開発ビルド: exe 隣接の `fonts/` ディレクトリからも読み込む。
- フォント未登録の場合、テキストは近似メトリクスのみ (グリフ描画なし) になる。

デスクトップ (PC/GDK) は従来どおり DirectWrite が主経路のため、フォント同梱は不要。
同様に、画像は stb_image、音声は dr_mp3/dr_wav/stb_vorbis が全プラットフォーム共通の
第一経路で、WIC / Media Foundation はデスクトップ限定のフォールバック
(AAC/TIFF 等の非標準形式向け)。コンソールでの動画 (`<video>`) は未対応。
