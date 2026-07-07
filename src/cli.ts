// CLI 引数の解析とヘルプ表示。process.argv を読み取り ctx を初期化する。
import pc from "picocolors";
import { ctx } from "./context.js";
import { RECOMMENDED_NODE_VERSION } from "./constants.js";

/**
 * @description ヘルプを出力して終了
 *              Output help and exit
 *
 * @return {void}
 * @method
 * @public
 */
export const echoHelp = (): void =>
{
    console.log();
    console.log(pc.green("`--platform` can be specified for macOS, Windows, iOS, Android, Xbox, and Web"));
    console.log(pc.green("It is not case sensitive."));
    console.log();
    console.log("For build example:");
    console.log("npx @next2d/builder --platform web --env prd");
    console.log();
    console.log("For preview example:");
    console.log("npx @next2d/builder --preview --platform web --env prd");
    console.log();
    console.log("For Xbox build example (prebuilt V8 is downloaded automatically):");
    console.log("npx @next2d/builder --platform xbox --env prd");
    console.log("To use your own V8 build: --v8-root C:\\path\\to\\v8");
    console.log();
    process.exit(1);
};

/**
 * @typedef {object} ParsedArgs
 */
export interface ParsedArgs {
    platform: string;
    environment: string;
    preview: boolean;
    open: boolean;
    build: boolean;
    v8Root: string;
    hasHelp: boolean;
}

/**
 * @description ビルド対象として受理するプラットフォーム名の集合。
 * @type {Set<string>}
 * @constant
 */
export const SUPPORTED_PLATFORMS: Set<string> = new Set([
    "windows", "macos", "linux",
    "steam:windows", "steam:macos", "steam:linux",
    "ios", "android", "xbox", "web"
]);

/**
 * @description process.argv 等のトークン列を解析する純関数 (副作用なし)。
 *              Parse an argv token list into structured flags (pure, no side effects).
 *
 * @param  {string[]} argv
 * @return {ParsedArgs}
 * @method
 * @public
 */
export const parseArgv = (argv: string[]): ParsedArgs =>
{
    const result: ParsedArgs = {
        "platform": "",
        "environment": "",
        "preview": false,
        "open": false,
        "build": false,
        "v8Root": "",
        "hasHelp": false
    };

    for (let idx: number = 0; idx < argv.length; ++idx) {

        switch (argv[idx]) {

            case "--preview":
                result.preview = true;
                break;

            case "--open":
                result.open = true;
                break;

            case "--build":
                result.build = true;
                break;

            case "--help":
            case "--h":
                result.hasHelp = true;
                break;

            case "--platform":
                result.platform = (argv[++idx] || "").toLowerCase();
                break;

            case "--env":
                result.environment = argv[++idx] || "";
                break;

            case "--v8-root":
                // Xbox ビルドで使う prebuilt V8 monolith のパス (環境変数 V8_ROOT より優先)
                result.v8Root = argv[++idx] || "";
                break;

            default:
                break;

        }

        if (result.hasHelp) {
            break;
        }
    }

    if (!result.platform || !result.environment) {
        result.hasHelp = true;
    }

    return result;
};

/**
 * @description プラットフォーム名をパス表現へ変換する ("steam:windows" -> "steam/windows")。
 *              Convert a platform name to its path form.
 *
 * @param  {string} platform
 * @return {string}
 * @method
 * @public
 */
export const derivePlatformDir = (platform: string): string =>
{
    return platform.indexOf(":") ? platform.split(":").join("/") : platform;
};

/**
 * @description Node バージョン検証 → process.argv 解析 → ctx 初期化 →
 *              プラットフォーム妥当性検証 → platformDir 算出 → 環境変数設定。
 *              不正/ヘルプ要求時は echoHelp() で終了する。
 *              Validate Node version, parse argv into ctx, validate platform,
 *              derive platformDir and export env vars. Exits via echoHelp on error.
 *
 * @return {void}
 * @method
 * @public
 */
export const initCli = (): void =>
{
    const version: string = process.versions.node;
    if (RECOMMENDED_NODE_VERSION > parseInt(version.split(".")[0])) {
        pc.red(`You are running Node Version:${version}.
View Generator requires Node ${RECOMMENDED_NODE_VERSION} or higher.
Please update your version of Node.`);
        process.exit(1);
    }

    const parsed: ParsedArgs = parseArgv(process.argv);

    ctx.platform    = parsed.platform;
    ctx.environment = parsed.environment;
    ctx.preview     = parsed.preview;
    ctx.open        = parsed.open;
    ctx.build       = parsed.build;
    ctx.v8Root      = parsed.v8Root;

    if (parsed.hasHelp || !SUPPORTED_PLATFORMS.has(ctx.platform)) {
        echoHelp();
    }

    ctx.platformDir = derivePlatformDir(ctx.platform);

    // update env variables
    process.env.NEXT2D_EBUILD_ENVIRONMENT = ctx.environment;
    process.env.NEXT2D_TARGET_PLATFORM    = ctx.platform;
};
