import { test } from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import {
    addAndroidProvider, addIosCheck, applyAndroidIntegrity, applyIosIntegrity,
    createNativeIntegrityLines, renderAndroidManifestSource, ANDROID_CHECK_SOURCE, IOS_MANIFEST_NAME
} from "../dist/native-integrity.js";

const sha = (text) => crypto.createHash("sha256").update(text).digest("hex");

const tempDir = (t) => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "next2d native integrity "));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    return root;
};

const ANDROID_MANIFEST = [
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>",
    "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">",
    "    <application android:label=\"@string/app_name\">",
    "        <activity android:name=\".MainActivity\" />",
    "    </application>",
    "</manifest>",
    ""
].join("\n");

const APP_DELEGATE = [
    "import UIKit",
    "import Capacitor",
    "",
    "@UIApplicationMain",
    "class AppDelegate: UIResponder, UIApplicationDelegate {",
    "",
    "    var window: UIWindow?",
    "",
    "    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {",
    "        // Override point for customization after application launch.",
    "        return true",
    "    }",
    "}",
    ""
].join("\n");

test("the list covers the scripts under public/ and the named config files, not media", (t) => {
    const base = tempDir(t);
    fs.mkdirSync(path.join(base, "public/assets/js"), { recursive: true });
    fs.writeFileSync(path.join(base, "public/index.html"), "<html>");
    fs.writeFileSync(path.join(base, "public/assets/js/app.js"), "app");
    fs.writeFileSync(path.join(base, "public/assets/bgm.mp3"), "sound");
    fs.writeFileSync(path.join(base, "capacitor.config.json"), "{}");
    fs.writeFileSync(path.join(base, "other.json"), "{}");
    assert.deepEqual(createNativeIntegrityLines(base, ["capacitor.config.json", "capacitor.plugins.json"]), [
        `${sha("{}")}  capacitor.config.json`,
        `${sha("app")}  public/assets/js/app.js`,
        `${sha("<html>")}  public/index.html`
    ].sort());
});

test("Android: the provider is added to the manifest once and the list is kept out of git", (t) => {
    const platform = tempDir(t);
    const manifestFile = path.join(platform, "app/src/main/AndroidManifest.xml");
    fs.mkdirSync(path.dirname(manifestFile), { recursive: true });
    fs.writeFileSync(manifestFile, ANDROID_MANIFEST);

    applyAndroidIntegrity(platform, [`${"a".repeat(64)}  public/index.html`]);
    const first = fs.readFileSync(manifestFile, "utf8");
    assert.match(first, /<provider\s+android:name="app\.next2d\.integrity\.Next2DAssetIntegrity"/);
    assert.ok(first.includes("android:authorities=\"${applicationId}.next2d.integrity\""));
    assert.ok(first.includes("android:exported=\"false\""));
    // inside <application>, and the rest of the manifest is untouched
    assert.ok(first.indexOf("<provider") < first.indexOf("</application>"));
    assert.equal(first.replace(/ {8}<!-- next2d-integrity:begin[\s\S]*?next2d-integrity:end -->\n/, ""), ANDROID_MANIFEST);

    const sourceDir = path.join(platform, "app/src/main/java/app/next2d/integrity");
    assert.equal(fs.readFileSync(path.join(sourceDir, "Next2DAssetIntegrity.java"), "utf8"), ANDROID_CHECK_SOURCE);
    assert.match(fs.readFileSync(path.join(sourceDir, "Next2DAssetIntegrityManifest.java"), "utf8"),
        new RegExp(`"${"a".repeat(64)}  public/index\\.html",`));
    assert.match(fs.readFileSync(path.join(sourceDir, ".gitignore"), "utf8"), /^Next2DAssetIntegrityManifest\.java$/m);

    // a second build changes only the list
    applyAndroidIntegrity(platform, null);
    assert.equal(fs.readFileSync(manifestFile, "utf8"), first);
    assert.equal(fs.readFileSync(path.join(sourceDir, "Next2DAssetIntegrityManifest.java"), "utf8"), renderAndroidManifestSource([]));
    assert.equal(addAndroidProvider(first), first);
    assert.throws(() => addAndroidProvider("<manifest></manifest>"), /no <\/application>/);
});

test("Android: the generated Java is well formed", () => {
    const source = renderAndroidManifestSource([`${"0".repeat(64)}  public/a "b" \\ c.js`]);
    assert.ok(source.includes(`        "${"0".repeat(64)}  public/a \\"b\\" \\\\ c.js",`));
    assert.match(source, /^package app\.next2d\.integrity;/);
    for (const text of [source, ANDROID_CHECK_SOURCE]) {
        assert.equal(text.split("{").length, text.split("}").length, "balanced braces");
        assert.equal(text.split("(").length, text.split(")").length, "balanced parentheses");
    }
    // nothing of the builder's template syntax may leak into the Java source
    assert.equal(ANDROID_CHECK_SOURCE.includes("${"), false);
    assert.match(ANDROID_CHECK_SOURCE, /Class\.forName\("app\.next2d\.integrity\.Next2DAssetIntegrityManifest"\)/);
});

test("iOS: the check is appended to AppDelegate.swift and called first at launch, once", (t) => {
    const patched = addIosCheck(APP_DELEGATE);
    const lines = patched.split("\n");
    const launch = lines.findIndex((line) => line.includes("didFinishLaunchingWithOptions"));
    assert.match(lines[launch + 1], /^ {8}Next2DAssetIntegrity\.verify\(\) \/\/ next2d-integrity:call/);
    assert.ok(patched.includes("enum Next2DAssetIntegrity {"));
    assert.ok(patched.includes("import CryptoKit"));
    // Swift string escapes survive the builder's template
    assert.ok(patched.includes("$0 == \"\\n\" || $0 == \"\\r\""));
    assert.ok(patched.includes("fatalError(\"The game files have been modified: \\(key)\")"));
    assert.equal(patched.includes("${"), false);
    const block = patched.slice(patched.indexOf("// next2d-integrity:begin"));
    assert.equal(block.split("{").length, block.split("}").length, "balanced braces");
    assert.equal(block.split("(").length, block.split(")").length, "balanced parentheses");
    // the original file is intact apart from the call and the appended block
    assert.ok(patched.startsWith(APP_DELEGATE.split("\n").slice(0, launch + 1).join("\n")));
    // a second build leaves the file as it is
    assert.equal(addIosCheck(patched), patched);
    assert.equal(patched.split("next2d-integrity:call").length, 2);
    assert.equal(patched.split("// next2d-integrity:begin").length, 2);
    assert.throws(() => addIosCheck("import UIKit\n"), /didFinishLaunchingWithOptions/);

    const platform = tempDir(t);
    const appDir = path.join(platform, "App/App");
    fs.mkdirSync(path.join(appDir, "public"), { recursive: true });
    fs.writeFileSync(path.join(appDir, "AppDelegate.swift"), APP_DELEGATE.replace(/\n/g, "\r\n"));
    applyIosIntegrity(platform, [`${"a".repeat(64)}  public/index.html`]);
    const written = fs.readFileSync(path.join(appDir, "AppDelegate.swift"), "utf8");
    // line endings of the file are kept
    assert.equal(written.replace(/\r\n/g, "").includes("\n"), false);
    assert.equal(written.replace(/\r\n/g, "\n"), patched);
    assert.equal(fs.readFileSync(path.join(appDir, "public", IOS_MANIFEST_NAME), "utf8"), `${"a".repeat(64)}  public/index.html\n`);
    // a development run removes the list, so nothing is checked
    applyIosIntegrity(platform, null);
    assert.equal(fs.existsSync(path.join(appDir, "public", IOS_MANIFEST_NAME)), false);
    assert.equal(fs.readFileSync(path.join(appDir, "AppDelegate.swift"), "utf8"), written);
});
