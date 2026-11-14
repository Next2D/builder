// platform-package モジュールの純関数テスト (委譲本体の buildWithPlatformPackage は対象外)。
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
    platformPackageName,
    resolvePlatformPackage,
    platformPackageEntry
} from "../dist/platform-package.js";
import { SUPPORTED_PLATFORMS, parseArgv } from "../dist/cli.js";

const makeTemp = (): string => fs.mkdtempSync(path.join(os.tmpdir(), "n2d-platform-"));

const writePackage = (dir: string, manifest: object): void => {
    fs.mkdirSync(dir, { "recursive": true });
    fs.writeFileSync(path.join(dir, "package.json"), JSON.stringify(manifest));
};

test("platformPackageName: @next2d/builder-<platform>", () => {
    assert.equal(platformPackageName("switch"), "@next2d/builder-switch");
});

test("cli: switch は受理されるプラットフォーム", () => {
    assert.equal(SUPPORTED_PLATFORMS.has("switch"), true);
    const r = parseArgv(["--platform", "Switch", "--env", "prd"]);
    assert.equal(r.platform, "switch");
    assert.equal(r.hasHelp, false);
});

test("resolvePlatformPackage: プロジェクトの node_modules から解決する", () => {
    const project = makeTemp();
    try {
        writePackage(project, { "name": "game" });
        const pkg = path.join(project, "node_modules", "@next2d", "builder-switch");
        writePackage(pkg, { "name": "@next2d/builder-switch", "main": "index.js" });
        fs.writeFileSync(path.join(pkg, "index.js"), "export const build = async () => {};");

        const resolved = resolvePlatformPackage("switch", project, {});
        assert.equal(fs.realpathSync(resolved as string), fs.realpathSync(pkg));
    } finally {
        fs.rmSync(project, { "recursive": true, "force": true });
    }
});

test("resolvePlatformPackage: 未インストールなら null", () => {
    const project = makeTemp();
    try {
        writePackage(project, { "name": "game" });
        assert.equal(resolvePlatformPackage("switch", project, {}), null);
    } finally {
        fs.rmSync(project, { "recursive": true, "force": true });
    }
});

test("resolvePlatformPackage: 環境変数の直接指定が優先される (相対パスはプロジェクト基準)", () => {
    const project = makeTemp();
    try {
        writePackage(project, { "name": "game" });
        const local = path.join(project, "local-package");
        writePackage(local, { "name": "@next2d/builder-switch" });

        assert.equal(
            resolvePlatformPackage("switch", project, { "NEXT2D_BUILDER_SWITCH_PATH": "local-package" }),
            local
        );
        // 指定先に package.json が無ければ null (node_modules へはフォールバックしない)
        assert.equal(
            resolvePlatformPackage("switch", project, { "NEXT2D_BUILDER_SWITCH_PATH": "missing" }),
            null
        );
    } finally {
        fs.rmSync(project, { "recursive": true, "force": true });
    }
});

test("platformPackageEntry: exports / main / index.js の順で決まる", () => {
    const dir = makeTemp();
    try {
        writePackage(dir, { "exports": { ".": "./a.js", "./package.json": "./package.json" }, "main": "b.js" });
        assert.equal(platformPackageEntry(dir), path.resolve(dir, "a.js"));

        writePackage(dir, { "exports": { ".": { "import": "./esm.js", "default": "./cjs.js" } } });
        assert.equal(platformPackageEntry(dir), path.resolve(dir, "esm.js"));

        writePackage(dir, { "exports": "./only.js" });
        assert.equal(platformPackageEntry(dir), path.resolve(dir, "only.js"));

        writePackage(dir, { "main": "b.js" });
        assert.equal(platformPackageEntry(dir), path.resolve(dir, "b.js"));

        writePackage(dir, {});
        assert.equal(platformPackageEntry(dir), path.resolve(dir, "index.js"));
    } finally {
        fs.rmSync(dir, { "recursive": true, "force": true });
    }
});
