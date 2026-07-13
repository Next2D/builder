// ビルド全体で共有する可変状態。CLI 解析結果 (platform/environment/フラグ) と
// loadConfig が設定するビルドディレクトリ等を保持し、各モジュールが参照/更新する。
// 単一のオブジェクトにまとめることで、ESM 間でも生きた状態を共有できる。
// Mutable build-wide state shared across modules via a single object.

/**
 * @typedef {object} BuildContext
 */
export interface BuildContext {
    /** ビルド対象プラットフォーム (例 "web" / "xbox" / "steam:windows") */
    platform: string;
    /** 環境識別子 (例 "prd" / "dev") */
    environment: string;
    /** --preview */
    preview: boolean;
    /** --open */
    open: boolean;
    /** --build */
    build: boolean;
    /** --v8-root (Xbox) */
    v8Root: string;
    /** vite の loadConfigFromFile 結果 */
    configObject: any | null;
    /** ビルド出力ディレクトリ (dist/<platformDir>/<env>) */
    buildDir: string;
    /** vite build.outDir (既定 "dist") */
    outDir: string;
    /** プラットフォームのパス表現 ("steam:windows" -> "steam/windows") */
    platformDir: string;
}

/**
 * @type {BuildContext}
 */
export const ctx: BuildContext = {
    "platform": "",
    "environment": "",
    "preview": false,
    "open": false,
    "build": false,
    "v8Root": "",
    "configObject": null,
    "buildDir": "",
    "outDir": "dist",
    "platformDir": ""
};
