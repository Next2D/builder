#!/usr/bin/env node

"use strict";

// エントリポイント。CLI を初期化し、Web ビルド → 各プラットフォーム書き出しを実行する。
// 各プラットフォームの実装は web/steam/native/xbox の各モジュールへ分割している。
import { ctx } from "./context.js";
import { initCli, echoHelp } from "./cli.js";
import { loadConfig, buildWeb } from "./web.js";
import { buildSteam } from "./steam.js";
import { runNative, openNative, buildNative } from "./native.js";
import { buildXbox } from "./xbox.js";

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
            await buildSteam();
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
        await loadConfig();
        await buildWeb();
        await multiBuild();
    } catch (error) {
        console.error(error);
        process.exit(1);
    }
};

execute();
