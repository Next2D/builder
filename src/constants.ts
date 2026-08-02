// ビルダー全体で使う定数群。
// Constants shared across the builder.

/**
 * @description 推奨 Node.js メジャーバージョン。
 * @type {number}
 * @constant
 */
export const RECOMMENDED_NODE_VERSION: number = 22;

/**
 * @type {string}
 * @constant
 */
export const CAPACITOR_CONFIG_NAME: string = "capacitor.config.json";

/**
 * @type {string}
 * @constant
 */
export const XBOX_DIR_NAME: string = "xbox";

/**
 * @type {string}
 * @constant
 */
export const XBOX_CONFIG_NAME: string = "MicrosoftGame.config";

/**
 * @description 自動ダウンロードする prebuilt V8 のバージョン。
 *              `.github/workflows/build-v8.yml` で発行した Releases のタグと、
 *              `xbox-host-ci.yml` の V8_TAG に一致させること。
 *              The prebuilt V8 version to auto-download. Must match the release tag
 *              published by `build-v8.yml` and V8_TAG in `xbox-host-ci.yml`.
 *
 * @type {string}
 * @constant
 */
export const XBOX_V8_VERSION: string = "13.7.152.19";

/**
 * @description prebuilt V8 のアセットリビジョン。同一 V8 バージョンでも
 *              gn フラグ変更でバイナリが変わる場合に上げ、Release タグと
 *              ローカルキャッシュを分離する。
 *              Asset revision. Bump when gn flags change for the same V8 version.
 *
 * @type {string}
 * @constant
 */
export const XBOX_V8_REVISION: string = "r3";

/**
 * @description ゲーム側の `xbox/` へスキャフォールドしないテンプレート内ファイル。
 *              - MicrosoftGame.config : ゲーム固有設定 (injectGameConfig が注入)
 *              - tests / .v8_headers  : builder リポジトリ側の開発用テスト・キャッシュ
 *              - build 系             : テンプレート内でビルドした場合の生成物
 *              Template entries excluded from the per-game `xbox/` scaffold.
 *
 * @type {Set<string>}
 * @constant
 */
export const XBOX_SCAFFOLD_EXCLUDES: Set<string> = new Set([
    XBOX_CONFIG_NAME,
    "tests",
    ".v8_headers",
    "build",
    "out",
    "_deps",
    "CMakeCache.txt",
    "CMakeFiles"
]);
