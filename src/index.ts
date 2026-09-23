#!/usr/bin/env node

"use strict";

// エントリポイント。CLI を初期化し、Web ビルド → 各プラットフォーム書き出しを実行する。
// 各プラットフォームの実装は web/steam/native/xbox の各モジュールへ分割している。
import { ctx } from "./context.js";
import { initCli, echoHelp } from "./cli.js";
import { loadConfig, buildWeb } from "./web.js";
import { buildElectron } from "./electron.js";
import { runNative, openNative, buildNative } from "./native.js";
import { buildXbox } from "./xbox.js";
import fs from "node:fs";
import path from "node:path";
import { readElectronConfig } from "./electron-config.js";
import { writeSharedSteamDepots } from "./steam.js";
import { uploadSteam } from "./steam-upload.js";

// Node バージョン検証・引数解析・ctx 初期化 (不正時はここで終了)。
initCli();

/**
 * @description プラットフォーム別のビルド実行関数
 *              Build execution dispatcher per platform
 *
 * @return {Promise}
 * @method
 * @public
 */
const multiBuild = async (): Promise<void> =>
{
    switch (ctx.platform) {

        case "windows":
        case "macos":
        case "linux":
        case "steam:windows":
        case "steam:macos":
        case "steam:linux":
            await buildElectron();
            break;

        case "ios":
        case "android":
            switch (true) {

                case ctx.open:
                    await openNative();
                    break;

                case ctx.build:
                    await buildNative();
                    break;

                case ctx.preview:
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
        if (ctx.steamUpload) {
            await uploadSteam({
                "root": process.cwd(), "steamRoot": ctx.steamRoot || "dist/steam", "environment": ctx.environment,
                "branch": ctx.steamBranch || undefined, "dryRun": ctx.dryRun
            });
            return;
        }
        await loadConfig();
        if (ctx.steamManifest) {
            const config = readElectronConfig(process.cwd());
            const version: string = JSON.parse(fs.readFileSync("package.json", "utf8")).version;
            const pending = writeSharedSteamDepots(path.resolve(ctx.outDir, "steam"), ctx.environment, config, version);
            if (pending.length) {
                throw new Error(`Shared Steam depots are incomplete. Export or collect all required OS packages first.\n${pending.join("\n")}`);
            }
            return;
        }
        await buildWeb();
        await multiBuild();
    } catch (error) {
        console.error(error);
        process.exit(1);
    }
};

execute();
