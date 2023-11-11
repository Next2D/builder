#!/usr/bin/env node

"use strict";

import pc from "picocolors";
import fs from "fs";
import cp from "child_process";
import { loadConfigFromFile } from "vite";
import { Platform, build as electronBuilder } from "electron-builder";

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
 * @type {string}
 * @constant
 */
const ELECTRON_BUILD_CONFIG_NAME: string = "electron.build.json";

/**
 * @type {string}
 * @constant
 */
const ELECTRON_INDEX_CONFIG_NAME: string = "electron.index.json";

/**
 * @return {Promise}
 * @method
 * @public
 */
const loadConfig = (): Promise<void> =>
{
    return new Promise(async (resolve): Promise<void> =>
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

        const config: any = await loadConfigFromFile(
            {
                "command": "build",
                "mode": "build"
            },
            `${process.cwd()}/vite.config.ts`
        );

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
 * @description Electronの書き出しに必要なconfigを生成
 *              Generate config for Electron export
 *
 * @return {object}
 * @method
 * @public
 */
const generateElectronConfig = (): any =>
{
    let config: any = {};

    const configPath: string = `${process.cwd()}/${ELECTRON_BUILD_CONFIG_NAME}`;
    if (!fs.existsSync(configPath)) {
        console.error(pc.red(`The file \`${ELECTRON_BUILD_CONFIG_NAME}\` could not be found.`));
        process.exit(1);
    }

    config = Object.assign(config,
        JSON.parse(fs.readFileSync(configPath, { "encoding": "utf8" }))
    );

    if (!config.appId) {
        console.log();
        console.log(pc.red("`appId` is not set."));
        console.log(`Please set \`appId\` in \`${ELECTRON_BUILD_CONFIG_NAME}\`.`);
        console.log();
        process.exit(1);
    }

    switch (platform) {

        case "windows":
        case "steam:windows":
            if (!("win" in config)) {
                config.win = {
                    "target": "portable"
                };
            }

            if ("mac" in config) {
                delete config.mac;
            }

            if ("linux" in config) {
                delete config.linux;
            }

            break;

        case "macos":
        case "steam:macos":
            if (!("mac" in config)) {
                config.mac = {
                    "target": "dmg"
                };
            }

            if ("win" in config) {
                delete config.win;
            }

            if ("linux" in config) {
                delete config.linux;
            }

            break;

        case "linux":
        case "steam:linux":
            if (!("linux" in config)) {
                config.linux = {
                    "target": "deb"
                };
            }

            if ("win" in config) {
                delete config.win;
            }

            if ("mac" in config) {
                delete config.mac;
            }

            break;

        default:
            break;

    }

    if (!("directories" in config)) {
        config.directories = {
            "output": "dist"
        };
    }

    if (!("files" in config)) {
        config.files = [];
    }

    config.files.push(`${$outDir}/${platformDir}/${environment}/`);
    config.directories.output = `${$outDir}/${platformDir}/build`;

    return config;
};

/**
 * @description Electronで読み込むindex.htmlのファイルパスをセット
 *              Set the file path of index.html to be loaded by Electron
 *
 * @return {void}
 * @method
 * @public
 */
const setElectronIndexPath = (): void =>
{
    const htmlFilePath: string = `${$buildDir}/index.html`;
    fs.writeFileSync(
        `${process.cwd()}/${ELECTRON_INDEX_CONFIG_NAME}`,
        JSON.stringify({
            "path": htmlFilePath.replace(process.cwd(), ".").trim()
        }, null, 2)
    );
};

/**
 * @description Electronで読み込むindex.htmlのファイルパスを初期化
 *              Initialize the file path of index.html to be loaded by Electron
 *
 * @return {void}
 * @method
 * @public
 */
const unlinkElectronIndexPath = (): void =>
{
    const jsonPath = `${process.cwd()}/${ELECTRON_INDEX_CONFIG_NAME}`;
    if (fs.existsSync(jsonPath)) {
        fs.unlinkSync(jsonPath);
    }
};

/**
 * @description Windows用アプリの書き出し関数
 *              Export function for Windows apps
 *
 * @return {void}
 * @method
 * @public
 */
const buildSteam = (): void =>
{
    /**
     * Electronで読み込むindex.htmlのパスをセット
     * Set the path to index.html to load in Electron
     */
    setElectronIndexPath();

    if (preview) {

        const packageJson = JSON.parse(
            fs.readFileSync(`${process.cwd()}/package.json`, { "encoding": "utf8" })
        );
        packageJson.type = "commonjs";

        fs.writeFileSync(
            `${process.cwd()}/package.json`,
            JSON.stringify(packageJson, null, 2)
        );

        cp.spawn(
            `${process.cwd()}/node_modules/.bin/electron`,
            [
                `${process.cwd()}/electron.js`
            ],
            { "stdio": "inherit" }
        );

        setTimeout((): void =>
        {
            if (packageJson.type !== "module") {
                packageJson.type = "module";

                // overwride
                fs.writeFileSync(
                    `${process.cwd()}/package.json`,
                    JSON.stringify(packageJson, null, 2)
                );
            }

            unlinkElectronIndexPath();
        }, 1000);

    } else {

        /**
         * Electronの書き出しに必要な設定を生成
         * Generate settings necessary for exporting Electron
         */
        const config: any = generateElectronConfig();

        console.log(pc.green("Start the `Electron` build process."));
        console.log();

        const buildObject: any = {
            "projectDir": process.cwd(),
            "config": config
        };

        switch (platform) {

            case "windows":
            case "steam:windows":
                buildObject.targets = Platform.WINDOWS.createTarget();
                break;

            case "macos":
            case "steam:macos":
                buildObject.targets = Platform.MAC.createTarget();
                break;

            case "linux":
            case "steam:linux":
                buildObject.targets = Platform.LINUX.createTarget();
                break;

            default:
                unlinkElectronIndexPath();
                console.log(pc.red("There is an error in the export platform settings."));
                console.log();
                process.exit(1);
                break;

        }

        electronBuilder(buildObject)
            .then((): void =>
            {
                unlinkElectronIndexPath();

                console.log(pc.green(`Finished building \`Electron\` for ${platform}.`));
                console.log();
            })
            .catch((error: any): void =>
            {
                unlinkElectronIndexPath();

                console.log(pc.red("Export of Electron failed."));
                console.log(pc.red(error));
            });

    }
};

/**
 * @description iOSのプロジェクトを生成
 *              Generate iOS project
 *
 * @return {Promise<void>}
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