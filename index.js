#!/usr/bin/env node

"use strict";

const version = process.versions.node;
if (15 > version.split(".")[0]) {
    console.error(
        "You are running Node Version:" + version + ".\n" +
        "this command requires Node 15 or higher. \n" +
        "Please update your version of Node."
    );
    process.exit(1);
}

const fs              = require("fs");
const cp              = require("child_process");
const webpackConfig   = require(`${process.cwd()}/webpack.config.js`);
const electronBuilder = require("electron-builder");
const chalk           = require("chalk");

let platform    = "";
let environment = "";
let hasHelp     = false;
let debug       = false;

for (let idx = 0; idx < process.argv.length; ++idx) {

    switch (process.argv[idx]) {

        case "--debug":
            debug = true;
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

if (hasHelp) {
    console.log();
    console.log("Export with `--platform` and `--env` arguments.");
    console.log();
    console.log("For build example:");
    console.log("npx @next2d/builder --platform web --env prd");
    console.log();
    console.log("For debug example:");
    console.log("npx @next2d/builder --debug --platform web --env prd");
    console.log();
    process.exit(1);
}

if (!fs.existsSync(`${webpackConfig.output.path}`)) {
    fs.mkdirSync(`${webpackConfig.output.path}`, { "recursive": true });
}

/**
 * @description 一時的に読み込みようのHTMLを生成
 *              Generate HTML for temporary loading
 *
 * @param  {string} file_path
 * @param  {string} product_name
 * @return {boolean}
 * @method
 * @public
 */
const buildHTML = (file_path, product_name) =>
{
    if (!fs.existsSync(file_path)) {

        fs.writeFileSync(
            file_path,
            `<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width, initial-scale=1.0, minimum-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <title>${product_name}</title>
    <script src="./app.js"></script>
</head>
<body style="margin: 0; padding: 0;">
</body>
</html>`);

        return true;
    }

    return false;
};

/**
 * @description Web用アプリの書き出し関数
 *              Export function for web applications
 *
 * @return {Promise}
 * @method
 * @public
 */
const buildWeb = () =>
{
    return new Promise((resolve) =>
    {
        const command = `${process.cwd()}/node_modules/.bin/webpack`;

        const mode = debug ? "development" : "production";
        const stream = cp.spawn(command, [
            "--mode",
            mode,
            "--env",
            environment
        ]);

        let errorCheck = false;
        stream.stderr.on("data", (error) =>
        {
            errorCheck = true;
            console.error(chalk.red(error.toString()));
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

            console.log(chalk.green("Build web app."));
            console.log();
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
const generateElectronConfig = (platform) =>
{
    let config = {};

    const configPath = `${process.cwd()}/electron.build.js`;
    if (!fs.existsSync(configPath)) {
        console.error(chalk.red("The file `electron.build.js` could not be found."));
        process.exit(1);
    }

    config = Object.assign(config, require(configPath));
    if (!config.appId) {
        console.log();
        console.log(chalk.red("`appId` is not set."));
        console.log("Please set `appId` in `electron.build.js`.");
        console.log();
        process.exit(1);
    }

    switch (platform) {

        case "windows":
            if (!("win" in config)) {
                config.win = {
                    "target": "nsis"
                };
            }

            if ("mac" in config) {
                delete config.mac;
            }

            break;

        case "macos":
            if (!("mac" in config)) {
                config.mac = {
                    "target": "dmg"
                };
            }

            if ("win" in config) {
                delete config.win;
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

    if (config.directories.output.slice(-1) !== "/") {
        config.directories.output += "/";
    }

    config.directories.output += `${platform}/${environment}`;

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
const setElectronIndexPath = () =>
{
    const htmlFilePath = `${webpackConfig.output.path}/index.html`;
    fs.writeFileSync(
        `${process.cwd()}/electron.index.json`,
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
const unlinkElectronIndexPath = () =>
{
    const jsonPath = `${process.cwd()}/electron.index.json`;
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
const buildWindow = () =>
{
    /**
     * web版の書き出し
     * Exporting the web version
     */
    buildWeb()
        .then(() =>
        {
            /**
             * Electronの書き出しに必要な設定を生成
             * Generate settings necessary for exporting Electron
             */
            const config = generateElectronConfig("windows");

            /**
             * 読み込みようのindex.htmlファイルがなければ生成
             * If there is no index.html file to load, generate one.
             */
            const htmlFile      = `${webpackConfig.output.path}/index.html`;
            const generatedHTML = buildHTML(htmlFile, config.productName);

            /**
             * Electronで読み込むindex.htmlのパスをセット
             * Set the path to index.html to load in Electron
             */
            setElectronIndexPath();

            if (debug) {

                const stream = cp.spawn(
                    `${process.cwd()}/node_modules/.bin/electron`,
                    [
                        `${process.cwd()}/electron.js`,
                        "--env",
                        environment
                    ]
                );

                stream.on("close", () =>
                {
                    if (generatedHTML) {
                        fs.unlinkSync(htmlFile);
                    }

                    unlinkElectronIndexPath();
                });

                stream.on("disconnect", () =>
                {
                    if (generatedHTML) {
                        fs.unlinkSync(htmlFile);
                    }

                    unlinkElectronIndexPath();
                });

            } else {

                electronBuilder
                    .build({
                        "projectDir": process.cwd(),
                        "targets": electronBuilder.Platform.WINDOWS.createTarget(),
                        "config": config
                    })
                    .then(() =>
                    {
                        if (generatedHTML) {
                            fs.unlinkSync(htmlFile);
                        }

                        unlinkElectronIndexPath();
                    });

            }
        });
};

/**
 * @description macOS用アプリの書き出し関数
 *              Export function for macOS apps
 *
 * @return {void}
 * @method
 * @public
 */
const buildMac = () =>
{
    /**
     * web版の書き出し
     * Exporting the web version
     */
    buildWeb()
        .then(() =>
        {
            /**
             * Electronの書き出しに必要な設定を生成
             * Generate settings necessary for exporting Electron
             */
            const config = generateElectronConfig("macos");

            /**
             * 読み込みようのindex.htmlファイルがなければ生成
             * If there is no index.html file to load, generate one.
             */
            const htmlFile      = `${webpackConfig.output.path}/index.html`;
            const generatedHTML = buildHTML(htmlFile, config.productName);

            /**
             * Electronで読み込むindex.htmlのパスをセット
             * Set the path to index.html to load in Electron
             */
            setElectronIndexPath();

            if (debug) {

                const stream = cp.spawn(
                    `${process.cwd()}/node_modules/.bin/electron`,
                    [
                        `${process.cwd()}/electron.js`,
                        "--env",
                        environment
                    ]
                );

                stream.on("close", () =>
                {
                    if (generatedHTML) {
                        fs.unlinkSync(htmlFile);
                    }

                    unlinkElectronIndexPath();
                });

                stream.on("disconnect", () =>
                {
                    if (generatedHTML) {
                        fs.unlinkSync(htmlFile);
                    }

                    unlinkElectronIndexPath();
                });

            } else {

                electronBuilder
                    .build({
                        "projectDir": process.cwd(),
                        "targets": electronBuilder.Platform.MAC.createTarget(),
                        "config": config
                    })
                    .then(() =>
                    {
                        if (generatedHTML) {
                            fs.unlinkSync(htmlFile);
                        }

                        unlinkElectronIndexPath();
                    });

            }
        });
};

/**
 * @description iOSのプロジェクトを生成
 *              Generate iOS project
 *
 * @return {Promise<void>}
 * @method
 * @public
 */
const generateNativeProject = () =>
{
    if (fs.existsSync(`${process.cwd()}/${platform}`)) {
        return Promise.resolve();
    }

    return new Promise((resolve) =>
    {
        const stream = cp.spawn("npx", ["cap", "add", platform]);

        let errorCheck = false;
        stream.stderr.on("data", (error) =>
        {
            errorCheck = true;
            console.error(chalk.red(error.toString()));
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

            console.log(chalk.green(`Successfully generated ${platform} project.`));
            console.log();

            resolve();
        });
    });
};

/**
 * @description iOS用アプリの書き出し関数
 *              Export function for iOS apps
 *
 * @return {void}
 * @method
 * @public
 */
const runNative = () =>
{
    generateNativeProject()
        .then(() =>
        {
            return buildWeb();
        })
        .then(() =>
        {
            /**
             * Capacitorの書き出しに必要な設定を生成
             * Generate settings necessary for exporting Capacitor
             */
            const config = JSON.parse(
                fs.readFileSync(`${process.cwd()}/capacitor.config.json`, { "encoding": "utf8" })
            );

            /**
             * 読み込みようのindex.htmlファイルがなければ生成
             * If there is no index.html file to load, generate one.
             */
            const htmlFile      = `${webpackConfig.output.path}/index.html`;
            const generatedHTML = buildHTML(htmlFile, config.appName);

            config.webDir = htmlFile
                .replace(process.cwd(), ".")
                .replace("index.html", "")
                .trim();

            fs.writeFileSync(
                `${process.cwd()}/capacitor.config.json`,
                JSON.stringify(config, null, 2)
            );

            return Promise.resolve(generatedHTML);

        })
        .then((generated_html) =>
        {
            process.on("exit", () =>
            {
                if (generated_html) {
                    fs.unlinkSync(`${webpackConfig.output.path}/index.html`);
                }
            });

            const { run } = require("@capacitor/cli");
            run();
        });
};

switch (platform) {

    case "windows":
        buildWindow();
        break;

    case "macos":
        buildMac();
        break;

    case "ios":
        if (debug) {
            runNative();
        }
        break;

    case "android":
        if (debug) {
            runNative();
        }
        break;

    case "web":
        buildWeb()
            .then(() =>
            {
                console.log("build done.");
            })
            .catch((error) => {
                console.error(error);
                process.exit(1);
            });
        break;

    default:
        console.log();
        console.log("`--platform` can be specified for macOS, Windows, iOS, Android, and Web");
        console.log("It is not case sensitive.");
        console.log();
        console.log("For example:");
        console.log("npx @next2d/builder --platform web --env prd");
        console.log();
        console.log("For Debug example:");
        console.log("npx @next2d/builder --debug --platform web --env prd");
        console.log();
        process.exit(1);
        break;

}