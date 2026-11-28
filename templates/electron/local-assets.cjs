const path = require("node:path");
const crypto = require("node:crypto");

const CSP = "default-src 'self' data: blob:; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; object-src 'none'; base-uri 'none'";
const resolveAssetPath = (root, requestURL) => {
    try {
        const url = new URL(requestURL);
        if (url.protocol !== "next2d:" || url.host !== "game" || url.username || url.password) {
            return null;
        }
        const name = decodeURIComponent(url.pathname);
        if (name.includes("\0") || name.includes("\\")) {
            return null;
        }
        const file = path.resolve(root, `.${name === "/" ? "/index.html" : name}`);
        const relative = path.relative(root, file);
        if (!relative || relative === ".." || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
            return null;
        }
        return file;
    } catch {
        return null;
    }
};
// The builder records the SHA-256 of every game file at build time (integrity.json, inside the ASAR).
// A file that was changed, added or replaced after the build does not match and must not be served.
const verifyAsset = (manifest, root, file, data) => {
    const name = path.relative(root, file).split(path.sep).join("/");
    const expected = Object.prototype.hasOwnProperty.call(manifest.files, name) ? manifest.files[name] : null;
    return typeof expected === "string"
        && crypto.createHash("sha256").update(data).digest("hex") === expected;
};
module.exports = { CSP, resolveAssetPath, verifyAsset };
