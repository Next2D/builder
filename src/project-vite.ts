import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";
import type * as Vite from "vite";

/** Resolve from the game, including when builder is launched through npx or a sibling checkout. */
export const loadProjectVite = async (root = process.cwd()): Promise<typeof Vite> => {
    const require = createRequire(path.join(root, "package.json"));
    let entry: string;
    try {
        entry = require.resolve("vite");
    } catch {
        throw new Error(`Vite is not installed in ${root}. Install the game's dependencies with npm install first.`);
    }
    const imported = await import(pathToFileURL(entry).href);
    // Vite 7's require condition resolves to its CJS API wrapper; Vite 8 is ESM.
    const vite = imported.default ?? imported;
    if (typeof vite.loadConfigFromFile !== "function") {
        throw new Error(`The Vite installation in ${root} does not expose loadConfigFromFile.`);
    }
    return vite;
};
