// xbox モジュールの純関数テスト: pak 直列化 / .rc 生成 / ディレクトリ走査。
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
    buildPak,
    renderAssetsRc,
    walkFiles,
    minifyJs
} from "../dist/xbox.js";

// host 側 EmbeddedAssets.cpp の ParseEmbeddedPak と同じ手順で pak を復号する検証用デコーダ。
const parsePak = (buf: Buffer): [string, Buffer][] => {
    assert.equal(buf.subarray(0, 4).toString("ascii"), "N2DA", "magic");
    assert.equal(buf.readUInt32LE(4), 1, "version");
    const count = buf.readUInt32LE(8);
    const out: [string, Buffer][] = [];
    let off = 12;
    for (let i = 0; i < count; ++i) {
        const kl = buf.readUInt32LE(off); off += 4;
        const key = buf.subarray(off, off + kl).toString("utf8"); off += kl;
        const dl = buf.readUInt32LE(off); off += 4;
        const data = buf.subarray(off, off + dl); off += dl;
        out.push([key, data]);
    }
    assert.equal(off, buf.length, "全バイトを消費する");
    return out;
};

test("buildPak: 往復 (UTF-8 キー/バイナリ含む)", () => {
    const entries: [string, Buffer][] = [
        ["app.js", Buffer.from("console.log('app');\n", "utf8")],
        ["assets/index-abc.js", Buffer.from("日本語テスト\n", "utf8")],
        ["js/bootstrap.js", Buffer.from([0x00, 0x01, 0xff, 0x7f])]
    ];
    const decoded = parsePak(buildPak(entries));
    assert.equal(decoded.length, entries.length);
    for (let i = 0; i < entries.length; ++i) {
        assert.equal(decoded[i][0], entries[i][0]);
        assert.ok(decoded[i][1].equals(entries[i][1]), `data[${i}] equal`);
    }
});

test("buildPak: 空 entries はヘッダのみ (12 バイト, count=0)", () => {
    const pak = buildPak([]);
    assert.equal(pak.length, 12);
    assert.equal(pak.readUInt32LE(8), 0);
    assert.deepEqual(parsePak(pak), []);
});

test("renderAssetsRc: ダブルクォート + フォワードスラッシュ + 末尾改行", () => {
    assert.equal(
        renderAssetsRc("D:\\a\\slime\\xbox\\assets.pak"),
        "N2DASSETS RCDATA \"D:/a/slime/xbox/assets.pak\"\n"
    );
    // 既にフォワードスラッシュならそのまま
    assert.equal(
        renderAssetsRc("/Users/x/xbox/assets.pak"),
        "N2DASSETS RCDATA \"/Users/x/xbox/assets.pak\"\n"
    );
});

test("walkFiles: 入れ子を posix 相対キーで収集", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "n2d-walk-"));
    try {
        fs.writeFileSync(path.join(dir, "app.js"), "a");
        fs.mkdirSync(path.join(dir, "assets"));
        fs.writeFileSync(path.join(dir, "assets", "x.js"), "b");
        fs.mkdirSync(path.join(dir, "assets", "nested"));
        fs.writeFileSync(path.join(dir, "assets", "nested", "y.js"), "c");

        const keys = walkFiles(dir).map(([k]: [string, string]): string => k).sort();
        assert.deepEqual(keys, ["app.js", "assets/nested/y.js", "assets/x.js"]);
    } finally {
        fs.rmSync(dir, { "recursive": true, "force": true });
    }
});

test("walkFiles: 存在しないディレクトリは空配列", () => {
    assert.deepEqual(walkFiles(path.join(os.tmpdir(), "n2d-does-not-exist-xyz")), []);
});

test("minifyJs: 圧縮しコメント除去、グローバル名は保持", () => {
    const src = `
// このコメントは除去される
globalThis.__next2d_boot = function () {
    var longLocalVariableName = 1 + 2;
    return longLocalVariableName;
};
`;
    const out = minifyJs("js/bootstrap.js", src);
    assert.ok(out.length < src.length, "縮む");
    assert.ok(!out.includes("除去される"), "コメント除去");
    assert.ok(out.includes("__next2d_boot"), "グローバル名は保持 (ゲームが参照)");
    assert.ok(!/\blongLocalVariableName\b/.test(out), "ローカル名はマングル");
});

test("minifyJs: 不正な JS は元コードのまま返す (埋め込み継続)", () => {
    const broken = "globalThis.x = (((;";
    assert.equal(minifyJs("js/bootstrap.js", broken), broken);
});
