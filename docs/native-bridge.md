# ネイティブ機能の拡張（Electron Native Bridge）

Next2Dのデスクトップアプリから、OS APIや任意のネイティブSDKを利用するための汎用拡張です。EOS専用ではありません。ネイティブ機能はアプリ側が用意する実行ファイル（sidecar）に実装し、builderが起動・通信・同梱を担当します。

対象はElectronのmacOS／Windows／LinuxとSteamデスクトップ出力です。WebブラウザやiOS／AndroidにはこのAPIは提供されません。モバイルは [Capacitorガイド](capacitor.md) を参照してください。

## 責務と構成

```text
Next2Dアプリ → window.next2dNative → Electron IPC → ネイティブ実行ファイル
                                                    └ OS API / 任意のSDK
```

- builder: CPU別ファイルの同梱、メソッド許可リスト、要求／応答・イベント通信、プロセス終了処理。
- アプリ: ネイティブ処理と入力検証、各OS向けコンパイル、SDKの初期化・後始末、ライセンスと権限設定。
- SDKの取得、ソースの自動コンパイル、認証・オンラインサービスの実装はbuilderの機能ではありません。
- `nativeBridge` 未設定ならプロセスを起動せず、`window.next2dNative` も公開しません。

## 1. ネイティブ実行ファイルを用意する

SDK不要の例として、OS情報を取得する [Swift実装](../examples/native-bridge/macos/main.swift) を同梱しています。Apple Silicon MacでXcode Command Line Toolsが利用できる場合、ゲームプロジェクトのルートで実行します。

```sh
mkdir -p native/macos-arm64
xcrun swiftc node_modules/@next2d/builder/examples/native-bridge/macos/main.swift -O -o native/macos-arm64/native-helper
```

ローカルのbuilderを使う場合はソースパスをその `examples/native-bridge/macos/main.swift` に置き換えます。このサンプルはmacOS用です。他のOSでは同じJSONプロトコルを実装した実行ファイルを用意します。使用言語はSwiftに限定されず、C++／Rustなどでも構いません。

## 2. electron.config.json に設定する

既存のアプリ設定に追加します。

```json
{
  "architectures": { "macos": "arm64" },
  "nativeBridge": {
    "methods": ["system.info"],
    "backgroundThrottling": true,
    "targets": {
      "macos-arm64": { "directory": "native/macos-arm64", "executable": "native-helper" }
    }
  }
}
```

| 設定 | 内容 |
|---|---|
| `methods` | rendererから許可するメソッド名。英字開始、英数字・`_`・`.`、最大64文字 |
| `targets` | ビルドするOS・CPUに対応するネイティブファイル群 |
| `directory` | プロジェクトルート基準のディレクトリ。絶対パスも可能。全体を同梱 |
| `executable` | ディレクトリ直下の実行ファイル名。パスやシェルコマンドは不可 |
| `backgroundThrottling` | `true` は非アクティブ時のrendererの処理抑制を許可。リアルタイム用途では `false`。既存利用との互換性のため省略時は `false` |

バックグラウンド設定はrendererのタイマー・描画の指定であり、OS全体の省電力設定やsidecarのスケジューリングを変更しません。

対応キーは `macos-arm64`／`macos-x64`／`macos-universal`／`windows-x64`／`windows-arm64`／`linux-x64`／`linux-arm64`。未用意のターゲットへの出力はエラーになります。macOS既定値はuniversalなので、上例はarm64を明示しています。universalは実行ファイルと依存ライブラリすべてが両CPU対応である必要があります。builderはCPU形式を検査・変換しません。

専用ディレクトリには実行ファイルと必要なdylib／DLL／so、配布可能なデータだけを置きます。ソース、秘密鍵、管理者資格情報を置かないでください。シンボリックリンクと特殊ファイルは拒否します。

## 3. Next2Dアプリから呼び出す

```js
const native = window.next2dNative;
if (native) {
    const unsubscribe = native.onEvent(({ event, data }) => {
        if (event === "system.ready") console.log("Native helper ready", data);
        if (event === "bridge.closed") console.error("Native helper closed", data);
    });
    try {
        const info = await native.request("system.info", {});
        console.log(info.platform, info.logicalProcessors);
    } catch (error) {
        console.error("Native request failed", error);
    } finally {
        unsubscribe(); // 継続監視する場合は画面・サービスの破棄時に解除
    }
}
```

`request(method, params)` は応答を返すPromise、`onEvent(listener)` は解除関数を返します。アプリの通信・デバイス操作層にラップし、UIとは分離してください。Node API・任意パス・汎用IPC・シェル実行はrendererへ公開しません。sandbox／contextIsolationを維持し、IPCはアプリのメインフレームに限定します。引数はsidecarでも検証してください。

## 4. ビルド・配布

```sh
npx @next2d/builder --platform macos --arch arm64 --env local --preview
npx @next2d/builder --platform macos --arch arm64 --env prd
```

ローカルのbuilderでは `npm run build` 後、ゲームプロジェクトから `node ../builder/dist/index.js ...` で実行できます。

ネイティブファイルはASAR外の `process.resourcesPath/native`（macOSでは `.app/Contents/Resources/native`）へ配置され、Web配信する `resources` とは分離されます。共有ライブラリは実行ファイル相対で解決できるようにビルドしてください。最初の `request` で同梱実行ファイルが起動します。

署名・公証は [デスクトップガイド](steam.md) の設定を使用し、実行ファイルと依存ライブラリを含む完成アプリで検証します。利用するOS機能に応じた説明文・権限・entitlement等は別途必要です。例えばLANアクセスでは `macos.localNetworkUsageDescription` を設定できます。単純なOS情報取得の例では必要ありません。

ビルド専用設定は `NEXT2D_ELECTRON_CONFIG_FILE=/absolute/path/to/config.json` で指定可能です。存在しない指定先はエラーになります。一時設定はアプリ側で管理し、配布ディレクトリには混ぜないでください。同梱ファイルを秘密にする保証はありません。

## Sidecarプロトコル v1

stdin／stdoutでUTF-8 JSONを1行ずつ交換します。stdoutはプロトコル専用とし、各行をflushしてください。

```json
{"id":1,"method":"system.info","params":{}}
{"id":1,"result":{"platform":"macos","logicalProcessors":8}}
{"id":2,"error":"Unsupported method"}
{"event":"system.ready","data":{"protocol":1}}
```

要求の整数IDを応答にそのまま返します。エラーは文字列の `error`、イベントは `event` と `data` を使います。`bridge.closed` はbuilder予約名です。業務エラーはその要求だけを失敗させます。

- 1行最大256KiB、同時要求128件、応答待ち15秒。SDK用途に固有のメソッドは予約しません。
- タイムアウトはネイティブ処理のキャンセルではありません。再試行・重複防止はアプリ側で設計します。
- プロトコル違反・プロセス終了で待機中の要求を失敗させ、`bridge.closed` を通知。自動再起動はしません。
- ブリッジの終了処理はstdinを閉じ、EOFでsidecarの後始末を促し、4秒後の強制終了を予約します。ただしElectron本体を4秒間終了待ちにする仕組みではなく、本体が先に終了するとタイマーは実行されません。必ずstdin EOFで速やかに終了する設計にしてください。プロトコル違反等では即時強制終了します。rendererクラッシュ時も終了処理を行いますが、強制終了・OS異常時の後始末は保証できません。
- 外部サービスの切断・保存等は必要に応じて終了前に独自のメソッドで行います。別の常駐プロセスを起動せず、stdin EOFでも終了するよう実装してください。
- stderrは読み捨て、rendererには転送しません。必要な診断ログはsidecar側で秘匿情報を除いて記録します。大量のイベントは発行元で制限・集約してください。

現状は1アプリにつき1つのsidecarです。複数SDKを使う場合はその実行ファイル内でメソッドを振り分けます。動的な任意実行や複数プロセスのプラグイン管理機構ではありません。

## ビルド時の準備スクリプト（任意）

`nativeBridge.prepare` にプロジェクト相対の `.js` / `.mjs` / `.cjs` を指定すると、通常のデスクトップ／Steam書き出しで、ネイティブ同梱前にNode.jsスクリプトを実行します。CLI・環境名・出力先は変わりません。未指定の場合は従来どおり `targets` のディレクトリ全体をコピーします。

```json
{ "nativeBridge": {
  "prepare": "scripts/prepare-native.mjs",
  "methods": ["system.info"],
  "targets": { "windows-x64": { "directory": "native/windows-x64", "executable": "helper.exe" } }
} }
```

実行契約：`node <script> --target <OS-CPU> --source <絶対パス> --destination <絶対パス>`。cwdはプロジェクトルート。出力先はビルド専用の一時フォルダで、スクリプトが実行ファイル・ライブラリ・必要な設定だけを書き込みます。元ディレクトリがまだ存在しない場合のコンパイルもスクリプト側で行えます。プロセスの環境変数を継承し、シェルは介しません。終了コード非0・120秒タイムアウト・シンボリックリンク・実行ファイル欠落でビルドを停止します。

このスクリプトはVite設定同様、信頼するプロジェクトのコードとして実行されます。OS権限を制限するサンドボックスではありません。スクリプト自体はプロジェクト外へ解決されるパスを拒否します。準備スクリプトのパスはrendererへ渡さず、準備済みファイルはWebアセットと分離します。一時フォルダは成功・失敗時に削除します。秘密情報をstdout/stderrへ出さないでください。

## 応用例: Epic Online Services（EOS）

EOSも上記と同じ汎用プロトコルを利用する一例です。builder内にEOS SDK・認証情報・Lobby/P2P実装はありません。

```json
{
  "nativeBridge": {
    "methods": ["eos.status", "match.open", "match.close", "lobby.create", "lobby.join", "p2p.send"],
    "backgroundThrottling": false,
    "targets": {
      "macos-arm64": { "directory": "native/eos/macos-arm64", "executable": "eos-sidecar" }
    }
  }
}
```

これらのメソッド名はアプリ側が定義する例で、builderの組み込みAPIではありません。アプリ側でEOS SDKを使うsidecarと各OS向けバイナリを作り、サービス設定・認証・退出・切断処理を実装します。rendererでは例えば `native.request("match.open", { protocol: 1, provider: "eos" })` をアプリ側の契約に合わせて呼び出します。

SDKによるネイティブ処理とゲーム固有の同期・命中・勝敗処理は分離します。サービス設定・利用条件・認証方式、複数端末接続、切断時の処理はアプリ側で検証が必要です。管理者やサーバー用の資格情報を同梱しないでください。この設定だけで別の通信方式がEOSへ自動移行することはありません。
