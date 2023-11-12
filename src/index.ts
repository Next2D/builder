#!/usr/bin/env node

"use strict";

import pc from "picocolors";
import fs from "fs";
import cp from "child_process";
import { loadConfigFromFile } from "vite";
import { api } from "@electron-forge/core";

const recommendeVersion: number = 18;
const version: string = process.versions.node;
if (recommendeVersion > parseInt(version.split(".")[0])) {
    pc.red(`You are running Node Version:${version}.
View Generator requires Node ${recommendeVersion} or higher.
Please update your version of Node.`);
    process.exit(1);
}

let platform: string    = "";
let environment: string = "";
let hasHelp: boolean    = false;
let preview: boolean    = false;

for (let idx: number = 0; idx < process.argv.length; ++idx) {

    switch (process.argv[idx]) {

        case "--preview":
            preview = true;
            break;

        case "--help":
        case "--h":
            hasHelp = true;
            break;

        case "--platform":
            platform = process.argv[++idx].toLowerCase();
            break;

        case "--env":
            environment = process.argv[++idx];
            break;

        default:
            break;

    }

    if (hasHelp) {
        break;
    }
}

if (!platform || !environment) {
    hasHelp = true;
}

/**
 * @description ヘルプを出力して終了
 *              Output help and exit
 *
 * @return {void}
 * @method
 * @public
 */
const echoHelp = (): void =>
{
    console.log();
    console.log(pc.green("`--platform` can be specified for macOS, Windows, iOS, Android, and Web"));
    console.log(pc.green("It is not case sensitive."));
    console.log();
    console.log("For build example:");
    console.log("npx @next2d/builder --platform web --env prd");
    console.log();
    console.log("For preview example:");
    console.log("npx @next2d/builder --preview --platform web --env prd");
    console.log();
    process.exit(1);
};

if (hasHelp) {
    echoHelp();
}

if (!platform || !environment) {
    echoHelp();
}

switch (platform) {

    case "windows":
    case "macos":
    case "linux":
    case "steam:windows":
    case "steam:macos":
    case "steam:linux":
    case "ios":
    case "android":
    case "web":
        break;

    default:
        echoHelp();
        break;

}

/**
 * @type {string}
 * @private
 */
const platformDir: string = platform.indexOf(":") ? platform.split(":").join("/") : platform;

/**
 * @type {object}
 * @private
 */
let $configObject: any | null = null;

/**
 * @type {string}
 * @private
 */
let $buildDir: string = "";

/**
 * @type {string}
 * @private
 */
let $outDir: string = "dist";

// update env variables
process.env.NEXT2D_EBUILD_ENVIRONMENT = environment;
process.env.NEXT2D_TARGET_PLATFORM    = platform;

/**
 * @type {string}
 * @constant
 */
const CAPACITOR_CONFIG_NAME: string = "capacitor.config.json";

/**
 * @return {Promise}
 * @method
 * @public
 */
const loadConfig = (): Promise<void> =>
{
    return new Promise((resolve): void =>
    {
        const packageJson = JSON.parse(
            fs.readFileSync(`${process.cwd()}/package.json`, { "encoding": "utf8" })
        );

        if (packageJson.type !== "module") {
            packageJson.type = "module";

            // overwride
            fs.writeFileSync(
                `${process.cwd()}/package.json`,
                JSON.stringify(packageJson, null, 2)
            );
        }

        const ext: string = fs.existsSync(`${process.cwd()}/vite.config.ts`) ? "ts" : "js";

        loadConfigFromFile(
            {
                "command": "build",
                "mode": "build"
            },
            `${process.cwd()}/vite.config.${ext}`
        )
            .then((config): void =>
            {
                // update config
                $configObject = config;
                $outDir       = $configObject.config?.build?.outDir || "dist";
                $buildDir     = `${process.cwd()}/${$outDir}/${platformDir}/${environment}`;

                if (!fs.existsSync(`${$buildDir}`)) {
                    fs.mkdirSync(`${$buildDir}`, { "recursive": true });
                    console.log(pc.green(`Create build directory: ${$buildDir}`));
                    console.log();
                }

                resolve();
            });
    });
};

/**
 * @description Web用アプリの書き出し関数
 *              Export function for web applications
 *
 * @return {Promise}
 * @method
 * @public
 */
const buildWeb = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
    {
        const stream = cp.spawn("npx", [
            "vite",
            "--outDir",
            $buildDir,
            "build"
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("Export of `HTML` and `JavaScript` failed.");
            }

            console.log();
            console.log(pc.green("`HTML` and `JavaScript` files are written out."));
            resolve();
        });
    });
};

/**
 * @description electronがインストールされてなければインストールを実行
 *              If electron is not installed, run install.
 *
 * @return {Promise}
 * @method
 * @public
 */
const installElectron = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
    {
        if (fs.existsSync(`${process.cwd()}/electron/node_modules`)) {
            return resolve();
        }

        const stream = cp.spawn("npm", [
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
 * @public
 */
const removeResources = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
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
 * @public
 */
const copyResources = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
    {
        const stream = cp.spawn("cp", [
            "-r",
            `${$buildDir}/`,
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
 * @description Windows用アプリの書き出し関数
 *              Export function for Windows apps
 *
 * @return {Promise}
 * @method
 * @public
 */
const buildSteam = async (): Promise<void> =>
{
    // reset
    await removeResources();

    // copy HTML, JavaScript
    await copyResources();

    if (preview) {

        cp.spawn("npx", [
            "electron",
            `${process.cwd()}/electron/index.js`
        ], { "stdio": "inherit" }
        );

    } else {

        await installElectron();

        console.log(pc.green("Start the `Electron` build process."));
        console.log();

        const packageOptions = {
            "dir": `${process.cwd()}/electron`,
            "outDir": `${$outDir}/${platformDir}/build`,
            "platform": "",
            "arch": "all"
        };

        switch (platform) {

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
                console.log(pc.green(`Finished building \`Electron\` for ${platform}.`));
                console.log();
            })
            .catch((error: any): void =>
            {
                console.log(pc.red("Export of Electron failed."));
                console.log(pc.red(error));
            });
    }
};

/**
 * @description iOSのプロジェクトを生成
 *              Generate iOS project
 *
 * @return {Promise}
 * @method
 * @public
 */
const generateNativeProject = (): Promise<void> =>
{
    return new Promise((resolve, reject): void =>
    {
        if (fs.existsSync(`${process.cwd()}/${platform}`)) {
            return resolve();
        }

        const stream = cp.spawn("npx", [
            "cap",
            "add",
            platform
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject(`Failed generated ${platform} project.`);
            }

            console.log(pc.green(`Successfully generated ${platform} project.`));
            console.log();

            resolve();
        });
    });
};

/**
 * @description iOS用アプリの書き出し関数
 *              Export function for iOS apps
 *
 * @return {Promise}
 * @method
 * @public
 */
const runNative = async (): Promise<void> =>
{
    await generateNativeProject();

    /**
     * Capacitorの書き出しに必要な設定を生成
     * Generate settings necessary for exporting Capacitor
     */
    const config = JSON.parse(
        fs.readFileSync(`${process.cwd()}/${CAPACITOR_CONFIG_NAME}`, { "encoding": "utf8" })
    );

    config.webDir = `${$outDir}/${platformDir}/${environment}/`;

    fs.writeFileSync(
        `${process.cwd()}/${CAPACITOR_CONFIG_NAME}`,
        JSON.stringify(config, null, 2)
    );

    cp.spawn("npx", [
        "cap",
        "run",
        platform
    ], { "stdio": "inherit" });
};

/**
 * @description ビルドの実行関数
 *              Build Execution Functions
 *
 * @return {void}
 * @method
 * @public
 */
const multiBuild = (): void =>
{
    switch (platform) {

        case "windows":
        case "macos":
        case "linux":
        case "steam:windows":
        case "steam:macos":
        case "steam:linux":
            buildSteam();
            break;

        case "ios":
        case "android":
            if (preview) {
                runNative();
            }
            break;

        case "web":
            console.log();
            break;

        default:
            echoHelp();
            break;

    }
};

/**
 * @description 実行関数
 *              function execution
 *
 * @return {Promise}
 * @method
 * @public
 */
const execute = async (): Promise<void> =>
{
    await loadConfig();

    buildWeb()
        .then(multiBuild)
        .catch((error): void => {
            console.log(pc.red(error));
            process.exit(1);
        });
};

execute();