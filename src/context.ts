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
    /** Electron CPU architecture (--arch); empty selects a platform default. */
    arch: string;
    /** Generate shared Steam depot manifests from existing packages only. */
    steamManifest: boolean;
    steamUpload: boolean;
    steamBranch: string;
    steamRoot: string;
    steamComment: string;
    dryRun: boolean;
    /** vite の loadConfigFromFile 結果 */
    configObject: Awaited<ReturnType<typeof import("vite").loadConfigFromFile>>;
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
    "arch": "",
    "steamManifest": false,
    "steamUpload": false,
    "steamBranch": "",
    "steamRoot": "",
    "steamComment": "",
    "dryRun": false,
    "configObject": null,
    "buildDir": "",
    "outDir": "dist",
    "platformDir": ""
};
