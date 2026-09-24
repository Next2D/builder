// Runs inside npx, where the downloaded tools' node_modules/.bin is on PATH.
import fs from "node:fs";
import path from "node:path";

const packages: Record<string, string> = JSON.parse(process.env.NEXT2D_TOOL_PACKAGES!);
let resolved: Record<string, string> | undefined;
for (const bin of (process.env.PATH ?? "").split(path.delimiter)) {
    if (path.basename(bin) !== ".bin") {
        continue;
    }
    const files: Record<string, string> = {};
    for (const [name, version] of Object.entries(packages)) {
        const file = path.join(path.dirname(bin), name, "package.json");
        if (fs.existsSync(file)) {
            const pkg = JSON.parse(fs.readFileSync(file, "utf8"));
            if (pkg.name === name && pkg.version === version) {
                files[name] = file;
            }
        }
    }
    // All SDKs must come from the same installation, with the requested exact versions.
    if (Object.keys(files).length === Object.keys(packages).length) {
        resolved = files;
        break;
    }
}
if (!resolved) {
    throw new Error("Could not resolve the requested build tools from npx's PATH.");
}
fs.writeFileSync(process.env.NEXT2D_TOOL_RESULT!, JSON.stringify(resolved));
