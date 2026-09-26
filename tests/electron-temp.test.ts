import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { withElectronPackagerTemp } from "../dist/electron-host.js";

test("concurrent Electron packagers own separate temporary roots", async () => {
    const directories: string[] = [];
    let release: () => void = () => {};
    const ready = new Promise<void>((resolve) => { release = resolve; });
    const build = () => withElectronPackagerTemp(async (dir: string) => {
        directories.push(dir);
        fs.writeFileSync(path.join(dir, "owned"), "test");
        if (directories.length === 2) release();
        await ready;
        assert.notEqual(directories[0], directories[1]);
        assert.equal(fs.readFileSync(path.join(dir, "owned"), "utf8"), "test");
        return dir;
    });
    assert.deepEqual(await Promise.all([build(), build()]), directories);
    assert.ok(directories.every((dir) => !fs.existsSync(dir)));
});

test("failed Electron packaging cleans only its own temporary root", async () => {
    let failed = "";
    await withElectronPackagerTemp(async (other: string) => {
        await assert.rejects(withElectronPackagerTemp(async (dir: string) => {
            failed = dir;
            throw new Error("packaging failed");
        }), /packaging failed/);
        assert.equal(fs.existsSync(failed), false);
        assert.equal(fs.existsSync(other), true);
    });
});
