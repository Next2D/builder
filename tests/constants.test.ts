// constants / context の健全性テスト。
import { test } from "node:test";
import assert from "node:assert/strict";
import {
    XBOX_V8_VERSION,
    XBOX_V8_REVISION,
    XBOX_SCAFFOLD_EXCLUDES,
    XBOX_CONFIG_NAME,
    CAPACITOR_CONFIG_NAME
} from "../dist/constants.js";
import { ctx } from "../dist/context.js";

test("XBOX_V8_VERSION は x.y.z.w 形式", () => {
    assert.match(XBOX_V8_VERSION, /^\d+\.\d+\.\d+\.\d+$/);
});

test("XBOX_V8_REVISION は r<number> 形式", () => {
    assert.match(XBOX_V8_REVISION, /^r\d+$/);
});

test("XBOX_SCAFFOLD_EXCLUDES は config とビルド生成物を除外", () => {
    assert.ok(XBOX_SCAFFOLD_EXCLUDES.has(XBOX_CONFIG_NAME));
    assert.ok(XBOX_SCAFFOLD_EXCLUDES.has("build"));
    assert.ok(XBOX_SCAFFOLD_EXCLUDES.has(".v8_headers"));
    assert.ok(XBOX_SCAFFOLD_EXCLUDES.has("CMakeCache.txt"));
});

test("設定ファイル名", () => {
    assert.equal(XBOX_CONFIG_NAME, "MicrosoftGame.config");
    assert.equal(CAPACITOR_CONFIG_NAME, "capacitor.config.json");
});

test("ctx の初期値", () => {
    assert.equal(ctx.outDir, "dist");
    assert.equal(ctx.platform, "");
    assert.equal(ctx.preview, false);
    assert.equal(ctx.configObject, null);
});
