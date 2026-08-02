// utils モジュールのテスト。
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import { getTemplateDir } from "../dist/utils.js";

test("getTemplateDir: パッケージ同梱 templates/<name> を指す絶対パス", () => {
    const dir = getTemplateDir("xbox");
    assert.ok(dir.endsWith("/templates/xbox") || dir.endsWith("\\templates\\xbox"), dir);
    // 実際にテンプレートが存在すること (リポジトリ同梱)
    assert.ok(fs.existsSync(dir), `template dir should exist: ${dir}`);
});
