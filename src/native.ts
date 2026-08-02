// iOS / Android (Capacitor) ビルド。
import pc from "picocolors";
import fs from "fs";
import { ctx } from "./context.js";
import { $spawn } from "./utils.js";
import { CAPACITOR_CONFIG_NAME } from "./constants.js";

/**
 * @description iOS/Androidのプロジェクトを生成
 *              Generate iOS/Android project
 *
 * @return {Promise}
 * @method
 * @private
 */
const generateNativeProject = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
    {
        if (fs.existsSync(`${process.cwd()}/${ctx.platform}`)) {
            return resolve();
        }

        const stream = $spawn("npx", [
            "cap",
            "add",
            ctx.platform
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject(`Failed generated ${ctx.platform} project.`);
            }

            console.log(pc.green(`Successfully generated ${ctx.platform} project.`));
            console.log();

            resolve();
        });
    });
};

/**
 * @description Capacitor の webDir を現在のビルド出力へ書き換える
 *              Point the Capacitor webDir at the current build output
 *
 * @return {void}
 * @method
 * @private
 */
const applyCapacitorWebDir = (): void =>
{
    const config = JSON.parse(
        fs.readFileSync(`${process.cwd()}/${CAPACITOR_CONFIG_NAME}`, { "encoding": "utf8" })
    );

    config.webDir = `${ctx.outDir}/${ctx.platformDir}/${ctx.environment}/`;

    fs.writeFileSync(
        `${process.cwd()}/${CAPACITOR_CONFIG_NAME}`,
        JSON.stringify(config, null, 2)
    );
};

/**
 * @description iOS用アプリの書き出し関数
 *              Export function for iOS apps
 *
 * @return {Promise}
 * @method
 * @public
 */
export const runNative = async (): Promise<void> =>
{
    await generateNativeProject();

    // Capacitorの書き出しに必要な設定を生成
    applyCapacitorWebDir();

    $spawn("npx", [
        "cap",
        "run",
        ctx.platform
    ], { "stdio": "inherit" });
};

/**
 * @description iOS/Androidアプリのオープン関数
 *              Open function for iOS/Android apps
 *
 * @return {Promise}
 * @method
 * @public
 */
export const openNative = async (): Promise<void> =>
{
    await generateNativeProject();

    // Capacitorの書き出しに必要な設定を生成
    applyCapacitorWebDir();

    const stream = $spawn("npx", [
        "cap",
        "sync",
        ctx.platform
    ], { "stdio": "inherit" });

    stream.on("close", (code: number): void =>
    {
        if (code !== 0) {
            console.log(pc.red(`Failed to sync ${ctx.platform} project.`));
            return;
        }

        $spawn("npx", [
            "cap",
            "open",
            ctx.platform
        ], { "stdio": "inherit" });
    });
};

/**
 * @description iOS/Androidアプリのビルド関数
 *              Build function for iOS/Android apps
 *
 * @return {Promise}
 * @method
 * @public
 */
export const buildNative = async (): Promise<void> =>
{
    await generateNativeProject();

    // Capacitorの書き出しに必要な設定を生成
    applyCapacitorWebDir();

    const stream = $spawn("npx", [
        "cap",
        "sync",
        ctx.platform
    ], { "stdio": "inherit" });

    stream.on("close", (code: number): void =>
    {
        if (code !== 0) {
            console.log(pc.red(`Failed to sync ${ctx.platform} project.`));
            return;
        }

        $spawn("npx", [
            "cap",
            "build",
            ctx.platform
        ], { "stdio": "inherit" });
    });
};
