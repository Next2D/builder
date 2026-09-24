import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const fixture = (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d tool paths "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    return root;
};

const installation = (root, directory, packages) => {
    const modules = path.join(root, directory, "node_modules");
    fs.mkdirSync(path.join(modules, ".bin"), { recursive: true });
    for (const [name, version] of Object.entries(packages)) {
        fs.mkdirSync(path.join(modules, name), { recursive: true });
        fs.writeFileSync(path.join(modules, name, "package.json"), JSON.stringify({ name, version }));
    }
    return path.join(modules, ".bin");
};

const resolve = (root, bins, packages) => spawnSync(process.execPath, [
    fileURLToPath(new URL("../dist/resolve-tool-packages.js", import.meta.url))
], {
    encoding: "utf8",
    env: {
        ...process.env, PATH: bins.join(path.delimiter),
        NEXT2D_TOOL_PACKAGES: JSON.stringify(packages), NEXT2D_TOOL_RESULT: path.join(root, "result.json")
    }
});

test("tool resolution skips unrelated project versions and returns the pinned npx installation", (t) => {
    const root = fixture(t);
    const old = installation(root, "game", { "@electron/packager": "18.4.4" });
    const pinned = installation(root, "npm cache", { "@electron/packager": "20.3.0" });
    const result = resolve(root, [old, pinned], { "@electron/packager": "20.3.0" });
    assert.equal(result.status, 0, result.stderr);
    const paths = JSON.parse(fs.readFileSync(path.join(root, "result.json"), "utf8"));
    assert.equal(paths["@electron/packager"], path.join(path.dirname(pinned), "@electron/packager/package.json"));
});

test("tool resolution rejects incomplete or mismatched SDK installations", (t) => {
    const root = fixture(t);
    const cli = installation(root, "first", { "@capacitor/cli": "8.5.2" });
    const sdk = installation(root, "second", { "@capacitor/core": "8.5.2" });
    const mismatched = installation(root, "third", { "@capacitor/cli": "8.5.2", "@capacitor/core": "8.0.0" });
    const result = resolve(root, [cli, sdk, mismatched], { "@capacitor/cli": "8.5.2", "@capacitor/core": "8.5.2" });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Could not resolve the requested build tools/);
    assert.equal(fs.existsSync(path.join(root, "result.json")), false);
});

test("loading platform modules works offline with an empty npm cache without acquiring tools", (t) => {
    const root = fixture(t);
    const cache = path.join(root, "empty-cache");
    const result = spawnSync(process.execPath, [
        "--input-type=module", "-e",
        `await import(${JSON.stringify(new URL("../dist/electron.js", import.meta.url).href)});
         await import(${JSON.stringify(new URL("../dist/native.js", import.meta.url).href)});`
    ], {
        cwd: root, encoding: "utf8", timeout: 10000,
        env: { ...process.env, npm_config_cache: cache, npm_config_offline: "true" }
    });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.doesNotMatch(result.stdout, /Preparing .* tools via npx/);
    assert.equal(fs.existsSync(cache), false);
});
