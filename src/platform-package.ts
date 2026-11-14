// 書き出しの実装を builder 本体ではなく別パッケージが持つプラットフォーム用の入口。
//
// 公開できない SDK に依存するプラットフォームは、書き出しの実装を builder に置けない。
// そこで builder は Web 資材のビルドまでを行い、残りをゲームのプロジェクトに
// インストールされた「プラットフォーム用パッケージ」へ委譲する。
//
//   ゲームのプロジェクト
//     package.json  …  devDependencies に @next2d/builder-<platform> を追加
//     $ npx @next2d/builder --platform <platform> --env prd
//
// パッケージは `build(context)` を named export する ESM であること。
import { ctx } from "./context.js";
import pc from "./colors.js";
import { getTemplateDir } from "./utils.js";
import { resolveXboxV8Root } from "./xbox.js";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";

/**
 * @description プラットフォーム用パッケージの build() へ渡す情報。
 *              Context handed to a platform package's build().
 * @typedef {object} PlatformBuildContext
 */
export interface PlatformBuildContext {
    /** 対象プラットフォーム名 (例 "switch") */
    platform: string;
    /** 環境識別子 (例 "prd") */
    environment: string;
    /** ゲームのプロジェクトルート (絶対パス) */
    projectDir: string;
    /** ビルド済み Web 資材のディレクトリ (index.html がある場所、絶対パス) */
    webDir: string;
    /** 書き出し先のルート (vite build.outDir、絶対パス) */
    outDir: string;
    /** builder 同梱のネイティブホストのテンプレート (共通コアのソース、絶対パス) */
    hostTemplateDir: string;
    /** ビルド済み V8 (Windows x64) を用意してそのパスを返す。初回はダウンロードする */
    resolveV8Root: () => Promise<string>;
    /** builder に渡された引数のうち builder が解釈しなかった残り (process.argv そのまま) */
    argv: string[];
}

/**
 * @description プラットフォーム名からパッケージ名を得る ("switch" -> "@next2d/builder-switch")。
 *              Package name for a platform.
 *
 * @param  {string} platform
 * @return {string}
 * @method
 * @public
 */
export const platformPackageName = (platform: string): string =>
{
    return `@next2d/builder-${platform}`;
};

/**
 * @description プラットフォーム用パッケージのディレクトリを解決する。
 *              1. 環境変数 NEXT2D_BUILDER_<PLATFORM>_PATH (開発用の直接指定)
 *              2. ゲームのプロジェクトから見える node_modules
 *              見つからなければ null。
 *              Resolve the platform package directory (env override, then the project's node_modules).
 *
 * @param  {string} platform
 * @param  {string} projectDir
 * @param  {NodeJS.ProcessEnv} [env=process.env]
 * @return {string | null}
 * @method
 * @public
 */
export const resolvePlatformPackage = (
    platform: string,
    projectDir: string,
    env: NodeJS.ProcessEnv = process.env
): string | null =>
{
    const override = env[`NEXT2D_BUILDER_${platform.toUpperCase()}_PATH`];
    if (override) {
        const dir = path.resolve(projectDir, override);
        return fs.existsSync(`${dir}/package.json`) ? dir : null;
    }

    try {
        const require = createRequire(path.join(projectDir, "package.json"));
        const manifest = require.resolve(`${platformPackageName(platform)}/package.json`);
        return path.dirname(manifest);
    } catch {
        return null;
    }
};

/**
 * @description パッケージの package.json からエントリファイルの絶対パスを得る
 *              (exports["."] / main / index.js の順)。
 *              Entry file of a package directory.
 *
 * @param  {string} packageDir
 * @return {string}
 * @method
 * @public
 */
export const platformPackageEntry = (packageDir: string): string =>
{
    const manifest = JSON.parse(fs.readFileSync(`${packageDir}/package.json`, "utf8"));
    const root = manifest.exports && manifest.exports["."];
    const entry: string = typeof manifest.exports === "string"
        ? manifest.exports
        : typeof root === "string"
            ? root
            : root && (root.import || root.default) || manifest.main || "index.js";
    return path.resolve(packageDir, entry);
};

/**
 * @description プラットフォーム用パッケージへ書き出しを委譲する。
 *              Delegate the export to the platform package installed in the project.
 *
 * @param  {string} platform
 * @return {Promise<void>}
 * @method
 * @public
 */
export const buildWithPlatformPackage = async (platform: string): Promise<void> =>
{
    const projectDir: string = process.cwd();
    const name: string = platformPackageName(platform);
    const packageDir = resolvePlatformPackage(platform, projectDir);

    if (!packageDir) {
        console.log();
        console.log(pc.red(`The export for "${platform}" is provided by a separate package: ${name}`));
        console.log(pc.red("It is not part of @next2d/builder and is only available to licensed developers of that platform."));
        console.log(pc.red("Add it to the devDependencies of this project, run `npm install`, and try again."));
        console.log();
        process.exit(1);
    }

    const plugin = await import(pathToFileURL(platformPackageEntry(packageDir)).href);
    if (typeof plugin.build !== "function") {
        throw new Error(`${name} does not export a build() function.`);
    }

    const context: PlatformBuildContext = {
        platform,
        "environment": ctx.environment,
        projectDir,
        "webDir": path.resolve(projectDir, ctx.buildDir),
        "outDir": path.resolve(projectDir, ctx.outDir),
        "hostTemplateDir": getTemplateDir("xbox"),
        "resolveV8Root": resolveXboxV8Root,
        "argv": process.argv.slice(2)
    };

    console.log(pc.green(`Delegating the "${platform}" export to ${name}`));
    await plugin.build(context);
};
