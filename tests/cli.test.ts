// cli モジュールの純関数テスト (副作用のある initCli は対象外)。
import { test } from "node:test";
import assert from "node:assert/strict";
import {
    parseArgv,
    derivePlatformDir,
    SUPPORTED_PLATFORMS
} from "../dist/cli.js";

test("parseArgv: platform / env の基本解析", () => {
    const r = parseArgv(["node", "cli", "--platform", "Web", "--env", "prd"]);
    assert.equal(r.platform, "web");        // 小文字化される
    assert.equal(r.environment, "prd");
    assert.equal(r.hasHelp, false);
    assert.equal(r.preview, false);
    assert.equal(r.open, false);
    assert.equal(r.build, false);
    assert.equal(r.v8Root, "");
});

test("parseArgv: フラグ (--preview/--open/--build)", () => {
    const r = parseArgv(["--platform", "ios", "--env", "dev", "--preview", "--open", "--build"]);
    assert.equal(r.preview, true);
    assert.equal(r.open, true);
    assert.equal(r.build, true);
    assert.equal(r.hasHelp, false);
});

test("parseArgv: --v8-root の値を取り込む", () => {
    const r = parseArgv(["--platform", "xbox", "--env", "prd", "--v8-root", "C:/v8"]);
    assert.equal(r.v8Root, "C:/v8");
});

test("parseArgv: platform か env が欠けると hasHelp=true", () => {
    assert.equal(parseArgv(["--platform", "web"]).hasHelp, true);
    assert.equal(parseArgv(["--env", "prd"]).hasHelp, true);
    assert.equal(parseArgv([]).hasHelp, true);
});

test("parseArgv: --help / --h", () => {
    assert.equal(parseArgv(["--help"]).hasHelp, true);
    assert.equal(parseArgv(["--h"]).hasHelp, true);
});

test("derivePlatformDir: コロンをスラッシュへ", () => {
    assert.equal(derivePlatformDir("steam:windows"), "steam/windows");
    assert.equal(derivePlatformDir("steam:macos"), "steam/macos");
    assert.equal(derivePlatformDir("web"), "web");
    assert.equal(derivePlatformDir("xbox"), "xbox");
});

test("SUPPORTED_PLATFORMS: 期待するプラットフォームを含む", () => {
    for (const p of ["windows", "macos", "linux", "steam:windows", "steam:macos", "steam:linux", "ios", "android", "xbox", "web"]) {
        assert.ok(SUPPORTED_PLATFORMS.has(p), `${p} should be supported`);
    }
    assert.equal(SUPPORTED_PLATFORMS.has("bogus"), false);
});
