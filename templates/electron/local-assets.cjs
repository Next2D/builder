const path = require("node:path");

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
module.exports = { CSP, resolveAssetPath };
