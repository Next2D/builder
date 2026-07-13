// Xbox (GDK ネイティブ) ビルド。V8 に Next2D の JS を載せ、Dawn(WebGPU/D3D12)で
// 描画する C++ ホストを、テンプレートのスキャフォールド → 資材配置 → pak 埋め込み →
// CMake/GDK ビルドの順で書き出す。
import pc from "picocolors";
import fs from "fs";
import os from "os";
import path from "path";
import cp from "child_process";
import { minifySync } from "vite";
import { ctx } from "./context.js";
import { getTemplateDir } from "./utils.js";
import {
    XBOX_DIR_NAME,
    XBOX_CONFIG_NAME,
    XBOX_V8_VERSION,
    XBOX_V8_REVISION,
    XBOX_SCAFFOLD_EXCLUDES
} from "./constants.js";

/**
 * @description XboxホストのC++/CMake一式をbuilder同梱テンプレートから毎回更新する。
 *              ホストは「エンジン相当」でユーザーは編集しない前提のため、常に最新へ上書きする。
 *              ただし各ゲーム固有の `MicrosoftGame.config` は対象外(injectGameConfigが扱う)。
 *              Refresh the Xbox host (C++/CMake) from the builder's bundled template.
 *
 * @return {Promise}
 * @method
 * @private
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
 *              未配置ならbuilder同梱の既定をフォールバックとして使い、作成を促す。
 *              Inject the game-root `MicrosoftGame.config` into the Xbox host.
 *
 * @return {Promise}
 * @method
 * @private
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
 * @private
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
            fs.cpSync(`${ctx.buildDir}/`, assetsDir, { "recursive": true });

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
export const walkFiles = (rootDir: string, prefix: string = ""): [string, string][] =>
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
 * @description (key, データ) の並びを exe 埋め込み用 pak バイナリへ直列化する純関数。
 *              host 側 EmbeddedAssets.cpp の ParseEmbeddedPak と対になる。
 *              フォーマット (リトルエンディアン uint32):
 *                magic "N2DA" / version(=1) / count / [keyLen,key,dataLen,data]...
 *              Serialize (key, data) entries into the embedded pak binary.
 *
 * @param  {Array<[string, Buffer]>} entries
 * @return {Buffer}
 * @method
 * @public
 */
export const buildPak = (entries: [string, Buffer][]): Buffer =>
{
    const chunks: Buffer[] = [];
    const header: Buffer = Buffer.alloc(12);
    header.write("N2DA", 0, "ascii");
    header.writeUInt32LE(1, 4);                 // version
    header.writeUInt32LE(entries.length, 8);    // count
    chunks.push(header);

    for (const [key, data] of entries) {
        const keyBuf: Buffer = Buffer.from(key, "utf8");
        const meta: Buffer = Buffer.alloc(4);
        meta.writeUInt32LE(keyBuf.length, 0);
        chunks.push(meta, keyBuf);
        const meta2: Buffer = Buffer.alloc(4);
        meta2.writeUInt32LE(data.length, 0);
        chunks.push(meta2, data);
    }

    return Buffer.concat(chunks);
};

/**
 * @description assets.pak を RCDATA "N2DASSETS" として取り込む .rc の内容を生成する純関数。
 *              相対パスだと rc.exe の CWD (build ディレクトリ) 基準で探して見つからない
 *              (RC2135) ため絶対パスを埋める。RC はフォワードスラッシュを受理する。
 *              文字列は必ずダブルクォート (RC 仕様。シングルクォートは不可)。
 *              Render the .rc content that embeds assets.pak as RCDATA.
 *
 * @param  {string} pakAbsPath  assets.pak の絶対パス
 * @return {string}
 * @method
 * @public
 */
export const renderAssetsRc = (pakAbsPath: string): string =>
{
    const forward: string = pakAbsPath.replace(/\\/g, "/");
    return `N2DASSETS RCDATA "${forward}"\n`;
};

/**
 * @description ホストスクリプト(js/bootstrap.js 等)を minify + 難読化する純関数。
 *              Oxc(vite 同梱)でコメント除去・空白圧縮・ローカル識別子マングルを行う。
 *              `globalThis.*` 等のグローバル名はゲームが参照するため保持される。
 *              失敗時は元コードをそのまま返す(埋め込み自体は継続)。
 *              Minify/obfuscate a host script; returns the original on failure.
 *
 * @param  {string} name  ファイル名 (拡張子で構文判定)
 * @param  {string} code  元の JavaScript
 * @return {string}
 * @method
 * @public
 */
export const minifyJs = (name: string, code: string): string =>
{
    try {
        const result = minifySync(name, code);
        if (result.errors && result.errors.length > 0) {
            const msg = (result.errors[0] as any)?.message ?? String(result.errors[0]);
            console.log(pc.yellow(`Xbox: minify skipped for ${name} (${msg}); embedding as-is.`));
            return code;
        }
        return result.code;
    } catch (error) {
        console.log(pc.yellow(`Xbox: minify failed for ${name}; embedding as-is. ${error}`));
        return code;
    }
};

/**
 * @description Xbox ホストの assets/app とホストスクリプト(js/bootstrap.js 等)を
 *              単一の pak バイナリへまとめ、`assets.pak` と RCDATA 参照用の
 *              `assets.rc` を xbox/ 直下へ生成する。CMake が assets.rc を検出して
 *              これらを exe 内リソースへ**必ず**埋め込み、平文 JS/HTML を配布物に残さない。
 *              埋め込みは必須(非埋め込みの選択肢は無い)。資材が無ければハードエラー。
 *
 *              pak フォーマット (リトルエンディアン uint32):
 *                magic "N2DA" / version(=1) / count / [keyLen,key,dataLen,data]...
 *              キーは assets/app 基準の posix 相対パス、または "js/bootstrap.js"。
 *              host 側 EmbeddedAssets.cpp の ParseEmbeddedPak と対になる。
 *
 * @return {Promise}
 * @method
 * @private
 */
const embedXboxAssets = (): Promise<void> =>
{
    return new Promise<void>((resolve, reject): void =>
    {
        const xboxDir: string   = `${process.cwd()}/${XBOX_DIR_NAME}`;
        const pakPath: string   = `${xboxDir}/assets.pak`;
        const rcPath: string    = `${xboxDir}/assets.rc`;

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

            // (key, データ) を読み込み、pak バイナリへ直列化する。
            // js/ のホストスクリプト(bootstrap.js/selftest.js)は minify + 難読化する。
            // assets/app の JS は vite が本番ビルドで minify 済みのためそのまま。
            let minified = 0;
            const pakEntries: [string, Buffer][] = entries.map(
                ([key, abs]): [string, Buffer] => {
                    if (key.startsWith("js/") && key.endsWith(".js")) {
                        const src = fs.readFileSync(abs, { "encoding": "utf8" });
                        const out = minifyJs(key, src);
                        if (out.length < src.length) {
                            ++minified;
                        }
                        return [key, Buffer.from(out, "utf8")];
                    }
                    return [key, fs.readFileSync(abs)];
                }
            );
            const pak: Buffer = buildPak(pakEntries);
            if (minified > 0) {
                console.log(pc.green(`Minified ${minified} host script(s) before embedding.`));
            }
            fs.writeFileSync(pakPath, pak);

            // rc.exe が assets.pak を RCDATA "N2DASSETS" として取り込む .rc を生成する。
            fs.writeFileSync(rcPath, renderAssetsRc(path.resolve(pakPath)), "utf8");

            console.log(pc.green(
                `Embedded ${entries.length} Xbox asset file(s) into assets.pak `
                + `(${(pak.length / 1024 / 1024).toFixed(2)} MB).`
            ));

            // 埋め込み済みの平文 `assets/`・`js/` は書き出し一覧から除外する。
            // これらは assets.pak (=exe 内 RCDATA) に格納済みで、CMake も埋め込みモードでは
            // exe 隣へステージしないため、xbox/ 直下に残しても不要かつ平文流出になる。
            fs.rmSync(`${xboxDir}/assets`, { "recursive": true, "force": true });
            fs.rmSync(`${xboxDir}/js`, { "recursive": true, "force": true });
            console.log(pc.green("Excluded plaintext `assets/` and `js/` from the export (embedded in exe)."));

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
 *              Resolve the V8 path for the Xbox build.
 *
 * @return {Promise<string>}
 * @method
 * @private
 */
const resolveXboxV8Root = async (): Promise<string> =>
{
    // 1. --v8-root 引数
    if (ctx.v8Root) {
        return path.resolve(process.cwd(), ctx.v8Root);
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
 *
 * @return {Promise}
 * @method
 * @public
 */
export const buildXbox = async (): Promise<void> =>
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
     */
    const gdkArch: string = process.env.NEXT2D_XBOX_ARCH || "Gaming.Xbox.Scarlett.x64";
    const cmakeBuildDir: string = `${process.cwd()}/${ctx.outDir}/${ctx.platformDir}/build`;

    /**
     * prebuilt V8 のパスを解決する (--v8-root > V8_ROOT > キャッシュ > 自動ダウンロード)。
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

    if (ctx.open || ctx.preview) {
        // Visual Studio ソリューションを開く (実機/エミュレータでの実行はVS側で行う)
        cp.spawn("cmake", [
            "--open", cmakeBuildDir
        ], { "stdio": "inherit" });
        return;
    }

    // フルビルド (Release パッケージまで)。失敗時は reject して非ゼロ終了させる
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
