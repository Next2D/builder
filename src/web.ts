// Web ビルド: vite 設定の読み込みと HTML/JavaScript の書き出し。
import pc from "./colors.js";
import fs from "fs";
import path from "node:path";
import { loadProjectVite } from "./project-vite.js";
import { ctx } from "./context.js";
import { $spawn } from "./utils.js";

/**
 * @description package.json / vite.config を読み取り、ビルドディレクトリ等を ctx へ設定
 *              Load vite config and populate build directories into ctx
 *
 * @return {Promise}
 * @method
 * @public
 */
export const loadConfig = async (): Promise<void> =>
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

    const { loadConfigFromFile } = await loadProjectVite();
    const config = await loadConfigFromFile(
        {
            "command": "build",
            "mode": "build"
        },
        `${process.cwd()}/vite.config.${ext}`
    );

    // update config
    ctx.configObject = config;
    ctx.outDir       = ctx.configObject?.config.build?.outDir || "dist";
    ctx.buildDir     = path.resolve(ctx.outDir, ctx.platformDir, ctx.environment);

    if (!fs.existsSync(`${ctx.buildDir}`)) {
        fs.mkdirSync(`${ctx.buildDir}`, { "recursive": true });
        console.log(pc.green(`Create build directory: ${ctx.buildDir}`));
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
export const buildWeb = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const stream = $spawn("npx", [
            "@next2d/vite-plugin-next2d-auto-loader"
        ], { "stdio": "inherit" });

        stream.once("error", reject);
        stream.on("close", (code: number): void =>
        {
            if (code !== 0) {
                reject("vite plugin command failed.");
                return;
            }

            const stream = $spawn("npx", [
                "vite",
                "--outDir",
                ctx.buildDir,
                "build"
            ], { "stdio": "inherit" });

            stream.once("error", reject);
            stream.on("close", (code: number): void =>
            {
                if (code !== 0) {
                    reject("Export of `HTML` and `JavaScript` failed.");
                    return;
                }

                console.log();
                console.log(pc.green("`HTML` and `JavaScript` files are written out."));
                resolve();
            });
        });
    });
};
