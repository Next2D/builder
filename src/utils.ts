// 汎用ユーティリティ (テンプレート解決 / 子プロセス起動)。
import path from "path";
import cp from "child_process";
import { fileURLToPath } from "url";

/**
 * @description builder パッケージに同梱したテンプレートの絶対パスを取得
 *              Get the absolute path of a template shipped with the builder package
 *
 * @param  {string} name
 * @return {string}
 * @method
 * @public
 */
export const getTemplateDir = (name: string): string =>
{
    // dist/*.js から見た templates/ (パッケージルート直下)
    const currentDir: string = path.dirname(fileURLToPath(import.meta.url));
    return path.resolve(currentDir, "..", "templates", name);
};

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
export const $spawn = (command: string, args: string[], options: object = {}): cp.ChildProcess =>
{
    if (process.platform === "win32" && (command === "npx" || command === "npm")) {
        const quoted: string[] = args.map((a: string): string => /\s/.test(a) ? `"${a}"` : a);
        return cp.spawn(command, quoted, { ...options, "shell": true });
    }
    return cp.spawn(command, args, options);
};
