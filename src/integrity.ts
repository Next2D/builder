// ビルドしたゲームのファイルが、配布後に書き換えられていないかを確かめるための一覧を作る。
// 一覧 (ファイルごとの SHA-256) は、各プラットフォームのホストに組み込み、ホストが読み込むたびに照合する。
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

export interface IIntegrityManifest {
    algorithm: "sha256";
    files: Record<string, string>;
}

export const hashFile = (file: string): string =>
    crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");

const walk = (directory: string): string[] =>
    fs.readdirSync(directory, { "withFileTypes": true })
        .flatMap((entry) => entry.isDirectory()
            ? walk(path.join(directory, entry.name))
            : entry.isFile() ? [path.join(directory, entry.name)] : []);

/**
 * directory 以下の全ファイルの一覧を作る。キーは directory からの相対パス (区切りは "/")。
 */
export const createIntegrityManifest = (directory: string): IIntegrityManifest => {
    const files: Record<string, string> = {};
    for (const file of walk(directory).sort()) {
        files[path.relative(directory, file).split(path.sep).join("/")] = hashFile(file);
    }
    return { "algorithm": "sha256", files };
};
