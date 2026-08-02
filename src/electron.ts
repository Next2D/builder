// Electron ベースのデスクトップ配布 (Steam / Epic Games Store など)。
import pc from "picocolors";
import fs from "fs";
import cp from "child_process";
import { api } from "@electron-forge/core";
import { ctx } from "./context.js";
import { $spawn } from "./utils.js";

/**
 * @description electronがインストールされてなければインストールを実行
 *              If electron is not installed, run install.
 *
 * @return {Promise}
 * @method
 * @private
 */
const installElectron = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        if (fs.existsSync(`${process.cwd()}/electron/node_modules`)) {
            return resolve();
        }

        const stream = $spawn("npm", [
            "--prefix",
            `${process.cwd()}/electron`,
            "install",
            `${process.cwd()}/electron`
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("`Electron` installation failed.");
            }

            console.log(pc.green("`Electron` successfully installed."));
            resolve();
        });
    });
};

/**
 * @description electronに含むリソースを初期化
 *              Initialize resources included in electron
 *
 * @return {Promise}
 * @method
 * @private
 */
const removeResources = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const stream = cp.spawn("rm", [
            "-rf",
            `${process.cwd()}/electron/resources/`
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("Failed to remove built resources.");
            }

            console.log(pc.green("Successfully remove built resources."));
            resolve();
        });
    });
};

/**
 * @description ビルドしたリソースをコピー
 *              Copy built resources
 *
 * @return {Promise}
 * @method
 * @private
 */
const copyResources = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const stream = cp.spawn("cp", [
            "-r",
            `${ctx.buildDir}/`,
            `${process.cwd()}/electron/resources`
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("Failed to copy built resources.");
            }

            console.log(pc.green("Successfully copy built resources."));
            resolve();
        });
    });
};

/**
 * @description Electron ベースのデスクトップアプリ書き出し関数
 *              (Windows / macOS / Linux、Steam・Epic Games Store 等の配布に共通)。
 *              Export function for Electron-based desktop apps.
 *
 * @return {Promise}
 * @method
 * @public
 */
export const buildElectron = async (): Promise<void> =>
{
    // reset
    await removeResources();

    // copy HTML, JavaScript
    await copyResources();

    if (ctx.preview) {

        $spawn("npx", [
            "electron",
            `${process.cwd()}/electron/index.js`
        ], { "stdio": "inherit" });

    } else {

        await installElectron();

        console.log(pc.green("Start the `Electron` build process."));
        console.log();

        const packageOptions = {
            "dir": `${process.cwd()}/electron`,
            "outDir": `${ctx.outDir}/${ctx.platformDir}/build`,
            "platform": "",
            "arch": "all"
        };

        switch (ctx.platform) {

            case "windows":
            case "steam:windows":
                packageOptions.platform = "win32";
                break;

            case "macos":
            case "steam:macos":
                packageOptions.platform = "mas";
                break;

            case "linux":
            case "steam:linux":
                packageOptions.platform = "linux";
                break;

            default:
                console.log(pc.red("There is an error in the export platform settings."));
                console.log();
                process.exit(1);

        }

        api
            .package(packageOptions)
            .then((): void =>
            {
                console.log(pc.green(`Finished building \`Electron\` for ${ctx.platform}.`));
                console.log();
            })
            .catch((error: any): void =>
            {
                console.log(pc.red("Export of Electron failed."));
                console.log(pc.red(error));
            });
    }
};
