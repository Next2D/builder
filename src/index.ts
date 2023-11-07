#!/usr/bin/env node

"use strict";

import pc from "picocolors";
import fs from "fs";
import cp from "child_process";
import { loadConfigFromFile } from "vite";
// import electronBuilder from "electron-builder";

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
// let debug: boolean      = false;

for (let idx: number = 0; idx < process.argv.length; ++idx) {

    switch (process.argv[idx]) {

        // case "--debug":
        //     debug = true;
        //     break;

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

if (hasHelp) {
    console.log();
    console.log(pc.green("Export with `--platform` and `--env` arguments."));
    console.log();
    console.log(pc.green("For build example:"));
    console.log(pc.green("npx @next2d/builder --platform web --env prd"));
    console.log();
    console.log(pc.green("For debug example:"));
    console.log(pc.green("npx @next2d/builder --debug --platform web --env prd"));
    console.log();
    process.exit(1);
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

// update env variables
process.env.NEXT2D_EBUILD_ENVIRONMENT = environment;
process.env.NEXT2D_TARGET_PLATFORM = platform;

/**
 * @return {Promise}
 * @method
 * @public
 */
const loadConfig = (): Promise<void> =>
{
    return new Promise(async (resolve): Promise<void> => 
    {
        const config: any = await loadConfigFromFile(
            {
                "command": "build",
                "mode": "build"
            },
            `${process.cwd()}/vite.config.ts`
        );

        $configObject = config;

        const outDir: string = $configObject.config?.build?.outDir || "dist";
        $buildDir = `${process.cwd()}/${outDir}/${platformDir}/${environment}`;

        if (!fs.existsSync(`${$buildDir}`)) {
            fs.mkdirSync(`${$buildDir}`, { "recursive": true });
            console.log(pc.green(`create build dir: ${$buildDir}`));
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
    return new Promise((resolve): void =>
    {
        const command = `${process.cwd()}/node_modules/.bin/vite`;

        const stream = cp.spawn(command, [
            "--outDir",
            $buildDir,
            "build"
        ]);

        let errorCheck = false;
        stream.stderr.on("data", (error) =>
        {
            errorCheck = true;
            console.error(pc.red(error.toString()));
        });

        stream.stdout.on("data", (data) =>
        {
            console.log(data.toString());
        });

        stream.on("exit", () =>
        {
            if (errorCheck) {
                process.exit(1);
            }

            console.log(pc.green("Build JavaScript File."));
            console.log();
            resolve();
        });

        resolve();
    });
};

// /**
//  * @description Electronの書き出しに必要なconfigを生成
//  *              Generate config for Electron export
//  *
//  * @return {object}
//  * @method
//  * @public
//  */
// const generateElectronConfig = (platform) =>
// {
//     let config = {};

//     const configPath = `${process.cwd()}/electron.build.js`;
//     if (!fs.existsSync(configPath)) {
//         console.error(chalk.red("The file `electron.build.js` could not be found."));
//         process.exit(1);
//     }

//     config = Object.assign(config, require(configPath));
//     if (!config.appId) {
//         console.log();
//         console.log(chalk.red("`appId` is not set."));
//         console.log("Please set `appId` in `electron.build.js`.");
//         console.log();
//         process.exit(1);
//     }

//     switch (platform) {

//         case "windows":
//             if (!("win" in config)) {
//                 config.win = {
//                     "target": "nsis"
//                 };
//             }

//             if ("mac" in config) {
//                 delete config.mac;
//             }

//             break;

//         case "macos":
//             if (!("mac" in config)) {
//                 config.mac = {
//                     "target": "dmg"
//                 };
//             }

//             if ("win" in config) {
//                 delete config.win;
//             }

//             break;

//         default:
//             break;

//     }

//     if (!("directories" in config)) {
//         config.directories = {
//             "output": "dist"
//         };
//     }

//     if (config.directories.output.slice(-1) !== "/") {
//         config.directories.output += "/";
//     }

//     config.directories.output += `${platform}/${environment}`;

//     return config;
// };

// /**
//  * @description Electronで読み込むindex.htmlのファイルパスをセット
//  *              Set the file path of index.html to be loaded by Electron
//  *
//  * @return {void}
//  * @method
//  * @public
//  */
// const setElectronIndexPath = () =>
// {
//     const htmlFilePath = `${webpackConfig.output.path}/index.html`;
//     fs.writeFileSync(
//         `${process.cwd()}/electron.index.json`,
//         JSON.stringify({
//             "path": htmlFilePath.replace(process.cwd(), ".").trim()
//         }, null, 2)
//     );
// };

// /**
//  * @description Electronで読み込むindex.htmlのファイルパスを初期化
//  *              Initialize the file path of index.html to be loaded by Electron
//  *
//  * @return {void}
//  * @method
//  * @public
//  */
// const unlinkElectronIndexPath = () =>
// {
//     const jsonPath = `${process.cwd()}/electron.index.json`;
//     if (fs.existsSync(jsonPath)) {
//         fs.unlinkSync(jsonPath);
//     }
// };

// /**
//  * @description Windows用アプリの書き出し関数
//  *              Export function for Windows apps
//  *
//  * @return {void}
//  * @method
//  * @public
//  */
// const buildWindow = () =>
// {
//     /**
//      * web版の書き出し
//      * Exporting the web version
//      */
//     buildWeb()
//         .then(() =>
//         {
//             /**
//              * Electronの書き出しに必要な設定を生成
//              * Generate settings necessary for exporting Electron
//              */
//             const config = generateElectronConfig("windows");

//             /**
//              * 読み込みようのindex.htmlファイルがなければ生成
//              * If there is no index.html file to load, generate one.
//              */
//             const htmlFile      = `${webpackConfig.output.path}/index.html`;
//             const generatedHTML = buildHTML(htmlFile, config.productName);

//             /**
//              * Electronで読み込むindex.htmlのパスをセット
//              * Set the path to index.html to load in Electron
//              */
//             setElectronIndexPath();

//             if (debug) {

//                 const stream = cp.spawn(
//                     `${process.cwd()}/node_modules/.bin/electron`,
//                     [
//                         `${process.cwd()}/electron.js`,
//                         "--env",
//                         environment
//                     ]
//                 );

//                 stream.on("close", () =>
//                 {
//                     if (generatedHTML) {
//                         fs.unlinkSync(htmlFile);
//                     }

//                     unlinkElectronIndexPath();
//                 });

//                 stream.on("disconnect", () =>
//                 {
//                     if (generatedHTML) {
//                         fs.unlinkSync(htmlFile);
//                     }

//                     unlinkElectronIndexPath();
//                 });

//             } else {

//                 electronBuilder
//                     .build({
//                         "projectDir": process.cwd(),
//                         "targets": electronBuilder.Platform.WINDOWS.createTarget(),
//                         "config": config
//                     })
//                     .then(() =>
//                     {
//                         if (generatedHTML) {
//                             fs.unlinkSync(htmlFile);
//                         }

//                         unlinkElectronIndexPath();
//                     });

//             }
//         });
// };

// /**
//  * @description macOS用アプリの書き出し関数
//  *              Export function for macOS apps
//  *
//  * @return {void}
//  * @method
//  * @public
//  */
// const buildMac = () =>
// {
//     /**
//      * web版の書き出し
//      * Exporting the web version
//      */
//     buildWeb()
//         .then(() =>
//         {
//             /**
//              * Electronの書き出しに必要な設定を生成
//              * Generate settings necessary for exporting Electron
//              */
//             const config = generateElectronConfig("macos");

//             /**
//              * 読み込みようのindex.htmlファイルがなければ生成
//              * If there is no index.html file to load, generate one.
//              */
//             const htmlFile      = `${webpackConfig.output.path}/index.html`;
//             const generatedHTML = buildHTML(htmlFile, config.productName);

//             /**
//              * Electronで読み込むindex.htmlのパスをセット
//              * Set the path to index.html to load in Electron
//              */
//             setElectronIndexPath();

//             if (debug) {

//                 const stream = cp.spawn(
//                     `${process.cwd()}/node_modules/.bin/electron`,
//                     [
//                         `${process.cwd()}/electron.js`,
//                         "--env",
//                         environment
//                     ]
//                 );

//                 stream.on("close", () =>
//                 {
//                     if (generatedHTML) {
//                         fs.unlinkSync(htmlFile);
//                     }

//                     unlinkElectronIndexPath();
//                 });

//                 stream.on("disconnect", () =>
//                 {
//                     if (generatedHTML) {
//                         fs.unlinkSync(htmlFile);
//                     }

//                     unlinkElectronIndexPath();
//                 });

//             } else {

//                 electronBuilder
//                     .build({
//                         "projectDir": process.cwd(),
//                         "targets": electronBuilder.Platform.MAC.createTarget(),
//                         "config": config
//                     })
//                     .then(() =>
//                     {
//                         if (generatedHTML) {
//                             fs.unlinkSync(htmlFile);
//                         }

//                         unlinkElectronIndexPath();
//                     });

//             }
//         });
// };

// /**
//  * @description iOSのプロジェクトを生成
//  *              Generate iOS project
//  *
//  * @return {Promise<void>}
//  * @method
//  * @public
//  */
// const generateNativeProject = () =>
// {
//     if (fs.existsSync(`${process.cwd()}/${platform}`)) {
//         return Promise.resolve();
//     }

//     return new Promise((resolve) =>
//     {
//         const stream = cp.spawn("npx", ["cap", "add", platform]);

//         let errorCheck = false;
//         stream.stderr.on("data", (error) =>
//         {
//             errorCheck = true;
//             console.error(chalk.red(error.toString()));
//         });

//         stream.stdout.on("data", (data) =>
//         {
//             console.log(data.toString());
//         });

//         stream.on("exit", () =>
//         {
//             if (errorCheck) {
//                 process.exit(1);
//             }

//             console.log(chalk.green(`Successfully generated ${platform} project.`));
//             console.log();

//             resolve();
//         });
//     });
// };

// /**
//  * @description iOS用アプリの書き出し関数
//  *              Export function for iOS apps
//  *
//  * @return {void}
//  * @method
//  * @public
//  */
// const runNative = () =>
// {
//     generateNativeProject()
//         .then(() =>
//         {
//             return buildWeb();
//         })
//         .then(() =>
//         {
//             /**
//              * Capacitorの書き出しに必要な設定を生成
//              * Generate settings necessary for exporting Capacitor
//              */
//             const config = JSON.parse(
//                 fs.readFileSync(`${process.cwd()}/capacitor.config.json`, { "encoding": "utf8" })
//             );

//             /**
//              * 読み込みようのindex.htmlファイルがなければ生成
//              * If there is no index.html file to load, generate one.
//              */
//             const htmlFile      = `${webpackConfig.output.path}/index.html`;
//             const generatedHTML = buildHTML(htmlFile, config.appName);

//             config.webDir = htmlFile
//                 .replace(process.cwd(), ".")
//                 .replace("index.html", "")
//                 .trim();

//             fs.writeFileSync(
//                 `${process.cwd()}/capacitor.config.json`,
//                 JSON.stringify(config, null, 2)
//             );

//             return Promise.resolve(generatedHTML);

//         })
//         .then((generated_html) =>
//         {
//             process.on("exit", () =>
//             {
//                 if (generated_html) {
//                     fs.unlinkSync(`${webpackConfig.output.path}/index.html`);
//                 }
//             });

//             const { run } = require("@capacitor/cli");
//             run();
//         });
// };

switch (platform) {

    // case "windows":
    //     buildWindow();
    //     break;

    // case "macos":
    //     buildMac();
    //     break;

    // case "ios":
    //     if (debug) {
    //         runNative();
    //     }
    //     break;

    // case "android":
    //     if (debug) {
    //         runNative();
    //     }
    //     break;

    case "web":
        loadConfig()
            .then((): void =>
            {
                buildWeb()
                    .then(() =>
                    {
                        console.log(pc.green("build done."));
                    })
                    .catch((error) => {
                        console.log(pc.red(error));
                        process.exit(1);
                    });
            });
        break;

    default:
        console.log();
        console.log(pc.green("`--platform` can be specified for macOS, Windows, iOS, Android, Steam:Windows, and Web"));
        console.log(pc.green("It is not case sensitive."));
        console.log();
        console.log("For example:");
        console.log("npx @next2d/builder --platform web --env prd");
        console.log();
        console.log("For Debug example:");
        console.log("npx @next2d/builder --debug --platform web --env dev");
        console.log();
        process.exit(1);
        break;

}