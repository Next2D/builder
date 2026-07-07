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

    let hasHelp: boolean = false;

    for (let idx: number = 0; idx < process.argv.length; ++idx) {

        switch (process.argv[idx]) {

            case "--preview":
                ctx.preview = true;
                break;

            case "--open":
                ctx.open = true;
                break;

            case "--build":
                ctx.build = true;
                break;

            case "--help":
            case "--h":
                hasHelp = true;
                break;

            case "--platform":
                ctx.platform = process.argv[++idx].toLowerCase();
                break;

            case "--env":
                ctx.environment = process.argv[++idx];
                break;

            case "--v8-root":
                // Xbox ビルドで使う prebuilt V8 monolith のパス (環境変数 V8_ROOT より優先)
                ctx.v8Root = process.argv[++idx] || "";
                break;

            default:
                break;

        }

        if (hasHelp) {
            break;
        }
    }

    if (!ctx.platform || !ctx.environment) {
        hasHelp = true;
    }

    if (hasHelp) {
        echoHelp();
    }

    switch (ctx.platform) {

        case "windows":
        case "macos":
        case "linux":
        case "steam:windows":
        case "steam:macos":
        case "steam:linux":
        case "ios":
        case "android":
        case "xbox":
        case "web":
            break;

        default:
            echoHelp();
            break;

    }

    ctx.platformDir = ctx.platform.indexOf(":") ? ctx.platform.split(":").join("/") : ctx.platform;

    // update env variables
    process.env.NEXT2D_EBUILD_ENVIRONMENT = ctx.environment;
    process.env.NEXT2D_TARGET_PLATFORM    = ctx.platform;
};
