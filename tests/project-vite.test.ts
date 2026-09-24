import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { loadProjectVite } from "../dist/project-vite.js";
import { minifyJs } from "../dist/xbox.js";

const fixture = (t, type = "module", source = "export const loadConfigFromFile = () => ({ config: { build: { outDir: 'game-output' } } });") => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d-project-vite-"));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const vite = path.join(root, "node_modules/vite");
    fs.mkdirSync(vite, { recursive: true });
    fs.writeFileSync(path.join(root, "package.json"), "{}");
    fs.writeFileSync(path.join(vite, "package.json"), JSON.stringify({ name: "vite", type, exports: "./index.js" }));
    fs.writeFileSync(path.join(vite, "index.js"), source);
    return root;
};

test("resolves the game's Vite instead of the builder's development dependency", async (t) => {
    const vite = await loadProjectVite(fixture(t));
    const result = await vite.loadConfigFromFile({ command: "build", mode: "build" });
    assert.equal(result.config.build.outDir, "game-output");
});

test("accepts the CJS API wrapper used by older Vite versions", async (t) => {
    const vite = await loadProjectVite(fixture(t, "commonjs", "module.exports = { loadConfigFromFile: () => 'cjs' };"));
    assert.equal(await vite.loadConfigFromFile({ command: "build", mode: "build" }), "cjs");
});

test("missing or broken project Vite reports a useful error instead of using builder's copy", async (t) => {
    const root = fixture(t);
    fs.rmSync(path.join(root, "node_modules"), { recursive: true, force: true });
    await assert.rejects(loadProjectVite(root), /Vite is not installed/);
    const broken = fixture(t, "module", "export const wrong = true;");
    await assert.rejects(loadProjectVite(broken), /does not expose loadConfigFromFile/);
});

test("Xbox uses the older Vite transform API when minifySync is unavailable", async (t) => {
    const original = process.cwd();
    const root = fixture(t, "commonjs", `module.exports = {
        loadConfigFromFile() {},
        transformWithEsbuild(code, name, options) {
            if (name !== "bootstrap.js" || !options.minify || options.legalComments !== "none") {
                throw new Error("Wrong minifier options");
            }
            return Promise.resolve({ code: "globalThis.result=3;" });
        }
    };`);
    process.chdir(root);
    try {
        assert.equal(await minifyJs("bootstrap.js", "globalThis.result = 1 + 2;"), "globalThis.result=3;");
    } finally {
        process.chdir(original);
    }
});
