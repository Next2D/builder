#!/usr/bin/env node

"use strict";

import pc from "picocolors";
import fs from "fs";
import os from "os";
import path from "path";
import { fileURLToPath } from "url";
import cp from "child_process";
import { loadConfigFromFile } from "vite";
import { api } from "@electron-forge/core";

const recommendeVersion: number = 22;
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
let open: boolean       = false;
let build: boolean      = false;
let v8Root: string      = "";

for (let idx: number = 0; idx < process.argv.length; ++idx) {

    switch (process.argv[idx]) {

        case "--preview":
            preview = true;
            break;

        case "--open":
            open = true;
            break;

        case "--build":
            build = true;
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

        case "--v8-root":
            // Xbox ビルドで使う prebuilt V8 monolith のパス (環境変数 V8_ROOT より優先)
            v8Root = process.argv[++idx] || "";
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
    case "xbox":
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
const loadConfig = async (): Promise<void> =>
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

    const config = await loadConfigFromFile(
        {
            "command": "build",
            "mode": "build"
        },
        `${process.cwd()}/vite.config.${ext}`
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
    return new Promise<void>((resolve, reject): void =>
    {
        const stream = $spawn("npx", [
            "@next2d/vite-plugin-next2d-auto-loader"
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("vite plugin command failed.");
            }

            const stream = $spawn("npx", [
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
 * @public
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
 * @public
 */
const copyResources = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
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

        const stream = $spawn("npx", [
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

    $spawn("npx", [
        "cap",
        "run",
        platform
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
const openNative = async (): Promise<void> =>
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

    const stream = $spawn("npx", [
        "cap",
        "sync",
        platform
    ], { "stdio": "inherit" });

    stream.on("close", (code: number): void =>
    {
        if (code !== 0) {
            console.log(pc.red(`Failed to sync ${platform} project.`));
            return;
        }

        $spawn("npx", [
            "cap",
            "open",
            platform
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
const buildNative = async (): Promise<void> =>
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

    const stream = $spawn("npx", [
        "cap",
        "sync",
        platform
    ], { "stdio": "inherit" });

    stream.on("close", (code: number): void =>
    {
        if (code !== 0) {
            console.log(pc.red(`Failed to sync ${platform} project.`));
            return;
        }

        $spawn("npx", [
            "cap",
            "build",
            platform
        ], { "stdio": "inherit" });
    });
};

/**
 * @type {string}
 * @constant
 */
const XBOX_DIR_NAME: string = "xbox";

/**
 * @description builder パッケージに同梱したテンプレートの絶対パスを取得
 *              Get the absolute path of a template shipped with the builder package
 *
 * @param  {string} name
 * @return {string}
 * @method
 * @public
 */
const getTemplateDir = (name: string): string =>
{
    // dist/index.js から見た templates/ (パッケージルート直下)
    const currentDir: string = path.dirname(fileURLToPath(import.meta.url));
    return path.resolve(currentDir, "..", "templates", name);
};

/**
 * @type {string}
 * @constant
 */
const XBOX_CONFIG_NAME: string = "MicrosoftGame.config";

/**
 * @description npx / npm を子プロセスとして起動する (クロスプラットフォーム)。
 *              Windows では npx/npm が .cmd (バッチファイル) のため、shell 経由で
 *              ないと起動できない (spawn ENOENT / Node の CVE-2024-27980 対応により
 *              .cmd の直接 spawn も不可)。スペースを含む引数は二重引用符で保護する。
 *              Spawn npx/npm cross-platform. On Windows they are .cmd batch files,
 *              which require a shell to spawn.
 *
 * @param  {string} command
 * @param  {array} args
 * @param  {object} options
 * @return {object}
 * @method
 * @public
 */
const $spawn = (command: string, args: string[], options: object = {}): cp.ChildProcess =>
{
    if (process.platform === "win32" && (command === "npx" || command === "npm")) {
        const quoted: string[] = args.map((a: string): string => /\s/.test(a) ? `"${a}"` : a);
        return cp.spawn(command, quoted, { ...options, "shell": true });
    }
    return cp.spawn(command, args, options);
};

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
const XBOX_V8_VERSION: string = "13.7.152.19";

/**
 * @description prebuilt V8 のアセットリビジョン。同一 V8 バージョンでも
 *              gn フラグ変更 (例: r2 = WebAssembly + DrumBrake 有効化) で
 *              バイナリが変わる場合に上げ、Release タグとローカルキャッシュを分離する。
 *              Asset revision. Bump when gn flags change for the same V8 version
 *              (r2 = WebAssembly + DrumBrake enabled).
 *
 * @type {string}
 * @constant
 */
const XBOX_V8_REVISION: string = "r3";

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
const XBOX_SCAFFOLD_EXCLUDES: Set<string> = new Set([
    XBOX_CONFIG_NAME,
    "tests",
    ".v8_headers",
    "build",
    "out",
    "_deps",
    "CMakeCache.txt",
    "CMakeFiles"
]);

/**
 * @description XboxホストのC++/CMake一式をbuilder同梱テンプレートから毎回更新する。
 *              ホストは「エンジン相当」でユーザーは編集しない前提のため、常に最新へ上書きする。
 *              ただし各ゲーム固有の `MicrosoftGame.config` は対象外(injectGameConfigが扱う)。
 *              Refresh the Xbox host (C++/CMake) from the builder's bundled template on every build.
 *              The host is engine-like and not user-edited, so it is always overwritten,
 *              excluding the per-game `MicrosoftGame.config` (handled by injectGameConfig).
 *
 * @return {Promise}
 * @method
 * @public
 */
const refreshXboxHost = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const projectDir: string = `${process.cwd()}/${XBOX_DIR_NAME}`;
        const templateDir: string = getTemplateDir("xbox");
        if (!fs.existsSync(templateDir)) {
            return reject(`Xbox template not found: ${templateDir}`);
        }

        try {
            fs.cpSync(templateDir, projectDir, {
                "recursive": true,
                // ゲーム固有設定と builder 側の開発用ファイルはスキャフォールドしない
                "filter": (src: string): boolean => !XBOX_SCAFFOLD_EXCLUDES.has(path.basename(src))
            });
            console.log(pc.green(`Successfully refreshed \`${XBOX_DIR_NAME}\` host.`));
            resolve();
        } catch (error) {
            reject(`Failed to refresh ${XBOX_DIR_NAME} host. ${error}`);
        }
    });
};

/**
 * @description ゲームルートの `MicrosoftGame.config` をXboxホストへ注入する。
 *              capacitor.config.json と同様、各ゲーム固有の設定(TitleId/StoreId/名称/ロゴ)は
 *              プロジェクトルートで管理し、それをホストのビルド対象へ配置する。
 *              未配置ならbuilder同梱の既定をフォールバックとして使い、作成を促す。
 *              Inject the game-root `MicrosoftGame.config` into the Xbox host.
 *
 * @return {Promise}
 * @method
 * @public
 */
const injectGameConfig = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const rootConfig: string = `${process.cwd()}/${XBOX_CONFIG_NAME}`;
        const hostConfig: string = `${process.cwd()}/${XBOX_DIR_NAME}/${XBOX_CONFIG_NAME}`;

        try {
            if (fs.existsSync(rootConfig)) {
                fs.cpSync(rootConfig, hostConfig);
                console.log(pc.green(`Applied project \`${XBOX_CONFIG_NAME}\`.`));
            } else {
                // フォールバック: builder同梱の既定を配置し、ルートへの作成を促す
                fs.cpSync(`${getTemplateDir("xbox")}/${XBOX_CONFIG_NAME}`, hostConfig);
                console.log(pc.yellow(`\`${XBOX_CONFIG_NAME}\` not found at project root. Using the default template.`));
                console.log(pc.yellow(`Create \`${XBOX_CONFIG_NAME}\` at your project root (like capacitor.config.json) to set TitleId/StoreId/logos.`));
            }
            resolve();
        } catch (error) {
            reject(`Failed to apply ${XBOX_CONFIG_NAME}. ${error}`);
        }
    });
};

/**
 * @description ビルド済みWeb資材(JS/HTML/アセット)をXboxホストのassetsへ配置
 *              Deploy built web resources (JS/HTML/assets) into the Xbox host assets
 *
 * @return {Promise}
 * @method
 * @public
 */
const copyXboxResources = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const assetsDir: string = `${process.cwd()}/${XBOX_DIR_NAME}/assets/app`;

        try {
            // reset
            fs.rmSync(assetsDir, { "recursive": true, "force": true });
            fs.mkdirSync(assetsDir, { "recursive": true });

            // copy built web resources (JS/HTML/assets)
            fs.cpSync(`${$buildDir}/`, assetsDir, { "recursive": true });

            console.log(pc.green("Successfully copy built resources to Xbox host."));
            resolve();
        } catch (error) {
            reject(`Failed to copy built resources to Xbox host. ${error}`);
        }
    });
};

/**
 * @description ディレクトリを再帰的に走査し、各ファイルの
 *              [posix 相対キー, 絶対パス] を集める。
 *              Walk a directory recursively, collecting [posixRelKey, absPath] pairs.
 *
 * @param  {string} rootDir 走査ルート
 * @param  {string} [prefix] キーへ付与する接頭辞 (例 "" / "assets/")
 * @return {Array<[string, string]>}
 * @method
 * @public
 */
const walkFiles = (rootDir: string, prefix: string = ""): [string, string][] =>
{
    const out: [string, string][] = [];
    if (!fs.existsSync(rootDir)) {
        return out;
    }
    for (const entry of fs.readdirSync(rootDir, { "withFileTypes": true })) {
        const abs: string = path.join(rootDir, entry.name);
        const key: string = prefix ? `${prefix}${entry.name}` : entry.name;
        if (entry.isDirectory()) {
            out.push(...walkFiles(abs, `${key}/`));
        } else if (entry.isFile()) {
            out.push([key, abs]);
        }
    }
    return out;
};

/**
 * @description Xbox ホストの assets/app とホストスクリプト(js/bootstrap.js 等)を
 *              単一の pak バイナリへまとめ、`assets.pak` と RCDATA 参照用の
 *              `assets.rc` を xbox/ 直下へ生成する。CMake が assets.rc を検出すると
 *              これらを exe 内リソースへ埋め込み、平文 JS/HTML を配布物に残さない。
 *              環境変数 `NEXT2D_XBOX_NO_EMBED` が設定されている場合は埋め込みを無効化し、
 *              既存の生成物を削除して隣接ファイル読み込みへ戻す。
 *
 *              pak フォーマット (リトルエンディアン uint32):
 *                magic "N2DA" / version(=1) / count / [keyLen,key,dataLen,data]...
 *              キーは assets/app 基準の posix 相対パス、または "js/bootstrap.js"。
 *              host 側 EmbeddedAssets.cpp の ParseEmbeddedPak と対になる。
 *
 * @return {Promise}
 * @method
 * @public
 */
const embedXboxAssets = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const xboxDir: string   = `${process.cwd()}/${XBOX_DIR_NAME}`;
        const pakPath: string   = `${xboxDir}/assets.pak`;
        const rcPath: string    = `${xboxDir}/assets.rc`;

        // 明示的に無効化された場合は生成物を消してフォールバックへ戻す。
        if (process.env.NEXT2D_XBOX_NO_EMBED) {
            try {
                fs.rmSync(pakPath, { "force": true });
                fs.rmSync(rcPath, { "force": true });
            } catch { /* ignore */ }
            console.log(pc.yellow("Xbox asset embedding disabled (NEXT2D_XBOX_NO_EMBED)."));
            return resolve();
        }

        try {
            // 収集: assets/app 一式 (キーは app 基準) + host スクリプト (js/ 基準)。
            const entries: [string, string][] = [];
            entries.push(...walkFiles(`${xboxDir}/assets/app`, ""));
            const bootstrap: string = `${xboxDir}/js/bootstrap.js`;
            if (fs.existsSync(bootstrap)) {
                entries.push(["js/bootstrap.js", bootstrap]);
            }
            const selftest: string = `${xboxDir}/js/selftest.js`;
            if (fs.existsSync(selftest)) {
                entries.push(["js/selftest.js", selftest]);
            }

            if (entries.length === 0) {
                return reject("No Xbox assets found to embed (assets/app is empty).");
            }

            // pak バイナリを組み立てる。
            const chunks: Buffer[] = [];
            const header: Buffer = Buffer.alloc(12);
            header.write("N2DA", 0, "ascii");
            header.writeUInt32LE(1, 4);                 // version
            header.writeUInt32LE(entries.length, 8);    // count
            chunks.push(header);

            for (const [key, abs] of entries) {
                const keyBuf: Buffer = Buffer.from(key, "utf8");
                const dataBuf: Buffer = fs.readFileSync(abs);
                const meta: Buffer = Buffer.alloc(4);
                meta.writeUInt32LE(keyBuf.length, 0);
                chunks.push(meta, keyBuf);
                const meta2: Buffer = Buffer.alloc(4);
                meta2.writeUInt32LE(dataBuf.length, 0);
                chunks.push(meta2, dataBuf);
            }

            fs.writeFileSync(pakPath, Buffer.concat(chunks));

            // rc.exe が assets.pak を RCDATA "N2DASSETS" として取り込む。
            // 相対パスだと rc.exe の CWD (build ディレクトリ) 基準で探して見つからない
            // (RC2135) ため、絶対パスを埋める。RC はフォワードスラッシュを受理する。
            // 文字列は必ずダブルクォート (RC 仕様。シングルクォートは不可)。
            const pakAbs: string = path.resolve(pakPath).replace(/\\/g, "/");
            fs.writeFileSync(rcPath, `N2DASSETS RCDATA "${pakAbs}"\n`, "utf8");

            const total: number = chunks.reduce((n, b): number => n + b.length, 0);
            console.log(pc.green(
                `Embedded ${entries.length} Xbox asset file(s) into assets.pak `
                + `(${(total / 1024 / 1024).toFixed(2)} MB).`
            ));
            resolve();
        } catch (error) {
            reject(`Failed to embed Xbox assets. ${error}`);
        }
    });
};

/**
 * @description Xbox ビルドに使う V8 のパスを解決する。優先順:
 *              1. `--v8-root` 引数
 *              2. 環境変数 `V8_ROOT`
 *              3. キャッシュ済みの prebuilt (%LOCALAPPDATA%/next2d/v8/<version>)
 *              4. GitHub Releases (build-v8.yml が発行) から自動ダウンロード
 *              3・4 により、通常は何も指定せずに `--platform xbox` だけでビルドできる。
 *              Resolve the V8 path for the Xbox build. With the cached/auto-downloaded
 *              prebuilt, no flag or env var is required in the common case.
 *
 * @return {Promise<string>}
 * @method
 * @public
 */
const resolveXboxV8Root = async (): Promise<string> =>
{
    // 1. --v8-root 引数
    if (v8Root) {
        return path.resolve(process.cwd(), v8Root);
    }

    // 2. 環境変数 V8_ROOT
    if (process.env.V8_ROOT) {
        return process.env.V8_ROOT;
    }

    // 3. キャッシュ済み prebuilt
    const cacheBase: string = process.env.LOCALAPPDATA
        ? `${process.env.LOCALAPPDATA}/next2d`
        : `${os.homedir()}/.cache/next2d`;
    const cacheDir: string = `${cacheBase}/v8/${XBOX_V8_VERSION}-${XBOX_V8_REVISION}`;
    if (fs.existsSync(`${cacheDir}/include/v8.h`)) {
        return cacheDir;
    }

    // 4. GitHub Releases から自動ダウンロード (マシンごとに初回のみ)
    const assetName: string = `v8-monolith-${XBOX_V8_VERSION}-windows-x64.zip`;
    const url: string = `https://github.com/Next2D/builder/releases/download/v8-${XBOX_V8_VERSION}-windows-x64-${XBOX_V8_REVISION}/${assetName}`;

    console.log(pc.green(`Downloading prebuilt V8 ${XBOX_V8_VERSION} (first time only) ...`));
    console.log(url);

    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`download failed: HTTP ${response.status}`);
    }

    fs.mkdirSync(cacheDir, { "recursive": true });
    const zipPath: string = `${cacheDir}/${assetName}.tmp`;
    fs.writeFileSync(zipPath, Buffer.from(await response.arrayBuffer()));

    // Windows 10+ 標準の tar (bsdtar) は zip も展開できる
    const extract = cp.spawnSync("tar", ["-xf", zipPath, "-C", cacheDir], { "stdio": "inherit" });
    fs.rmSync(zipPath, { "force": true });
    if (extract.status !== 0 || !fs.existsSync(`${cacheDir}/include/v8.h`)) {
        fs.rmSync(cacheDir, { "recursive": true, "force": true });
        throw new Error("failed to extract the prebuilt V8 archive");
    }

    console.log(pc.green(`Prebuilt V8 cached at: ${cacheDir}`));
    return cacheDir;
};

/**
 * @description Xbox(GDKネイティブ)用アプリの書き出し関数。
 *              V8にNext2DのJSを載せ、Dawn(WebGPU/D3D12)で描画するC++ホストをビルドする。
 *              Export function for Xbox (GDK native) apps.
 *              Builds a C++ host that runs Next2D's JS on V8 and renders via Dawn (WebGPU/D3D12).
 *
 * @return {Promise}
 * @method
 * @public
 */
const buildXbox = async (): Promise<void> =>
{
    // C++/CMake ホストを最新へ更新 (ゲーム固有設定は除外)
    await refreshXboxHost();
    // ゲームルートの MicrosoftGame.config を注入
    await injectGameConfig();
    // ビルド済みWeb資材を配置
    await copyXboxResources();
    // assets/app + host スクリプトを exe 埋め込み用 pak (+ rc) へまとめる
    await embedXboxAssets();

    /**
     * GDK ビルドは Windows + Visual Studio + Microsoft GDK が必須。
     * それ以外の環境ではスキャフォールドと資材配置のみを行い、手順を案内する。
     * The GDK build requires Windows + Visual Studio + Microsoft GDK.
     * On other platforms we only scaffold and copy assets, then guide the next steps.
     */
    if (process.platform !== "win32") {
        console.log();
        console.log(pc.yellow("Xbox host project has been generated and assets were copied."));
        console.log(pc.yellow("The GDK build must run on Windows with Visual Studio and the Microsoft GDK installed."));
        console.log(pc.yellow(`See ${XBOX_DIR_NAME}/README.md for the build steps.`));
        console.log();
        return;
    }

    /**
     * @type {string}
     * GDK の対象コンソール世代。既定は Xbox Series X|S (Scarlett)。
     * 必要に応じて `Gaming.Xbox.XboxOne.x64` / `Gaming.Desktop.x64` を選択。
     */
    const gdkArch: string = process.env.NEXT2D_XBOX_ARCH || "Gaming.Xbox.Scarlett.x64";
    const cmakeBuildDir: string = `${process.cwd()}/${$outDir}/${platformDir}/build`;

    /**
     * prebuilt V8 のパスを解決する (--v8-root > V8_ROOT > キャッシュ > 自動ダウンロード)。
     * 通常は何も指定せずにビルドできる。
     */
    let resolvedV8Root: string = "";
    try {
        resolvedV8Root = await resolveXboxV8Root();
    } catch (error) {
        console.log(pc.red(`Failed to prepare the prebuilt V8: ${error}`));
        console.log(pc.red("Fallbacks:"));
        console.log(pc.red("  1) Retry later (the download may have failed temporarily)."));
        console.log(pc.red("  2) Build V8 yourself and pass `--v8-root <path>`:"));
        console.log(pc.red("     npx @next2d/builder --platform xbox --env prd --v8-root C:\\path\\to\\v8"));
        console.log(pc.red(`     See ${XBOX_DIR_NAME}/README.md ("V8 の用意") for the build steps.`));
        return;
    }
    if (!fs.existsSync(`${resolvedV8Root}/include/v8.h`)) {
        console.log(pc.red(`\`include/v8.h\` not found under: ${resolvedV8Root}`));
        console.log(pc.red(`Check the \`--v8-root\` path. See ${XBOX_DIR_NAME}/README.md for how to prepare the prebuilt V8.`));
        return;
    }

    const configure = (): Promise<void> =>
    {
        return new Promise<void>((resolve, reject): void =>
        {
            const stream = cp.spawn("cmake", [
                "-S", `${process.cwd()}/${XBOX_DIR_NAME}`,
                "-B", cmakeBuildDir,
                "-G", "Visual Studio 17 2022",
                "-A", gdkArch,
                "-D", `V8_ROOT=${resolvedV8Root}`
            ], { "stdio": "inherit" });

            stream.on("close", (code: number): void =>
            {
                if (code !== 0) {
                    return reject("Failed to configure the Xbox (CMake/GDK) project.");
                }
                resolve();
            });
        });
    };

    await configure();

    if (open || preview) {
        // Visual Studio ソリューションを開く (実機/エミュレータでの実行はVS側で行う)
        cp.spawn("cmake", [
            "--open", cmakeBuildDir
        ], { "stdio": "inherit" });
        return;
    }

    // フルビルド (Release パッケージまで)。失敗時は reject して非ゼロ終了させる
    // (以前は表示のみで exit code 0 のまま成功扱いになっていた)
    await new Promise<void>((resolve, reject): void =>
    {
        const stream = cp.spawn("cmake", [
            "--build", cmakeBuildDir,
            "--config", "Release"
        ], { "stdio": "inherit" });

        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                return reject("Export of the Xbox (GDK) package failed.");
            }
            console.log();
            console.log(pc.green(`Finished building the Xbox (GDK) package for ${gdkArch}.`));
            console.log();
            resolve();
        });
    });
};

/**
 * @description ビルドの実行関数
 *              Build Execution Functions
 *
 * @return {void}
 * @method
 * @public
 */
const multiBuild = async (): Promise<void> =>
{
    switch (platform) {

        case "windows":
        case "macos":
        case "linux":
        case "steam:windows":
        case "steam:macos":
        case "steam:linux":
            await buildSteam();
            break;

        case "ios":
        case "android":
            switch (true) {

                case open:
                    await openNative();
                    break;

                case build:
                    await buildNative();
                    break;

                case preview:
                    await runNative();
                    break;

                default:
                    break;
            }
            break;

        case "xbox":
            await buildXbox();
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
    try {
        await loadConfig();
        await buildWeb();
        await multiBuild();
    } catch (error) {
        console.error(error);
        process.exit(1);
    }
};

execute();