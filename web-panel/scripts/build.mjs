import { mkdir, copyFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const deployDir = join(root, "deploy");

await mkdir(deployDir, { recursive: true });

await build({
  entryPoints: [join(root, "src", "worker.js")],
  outfile: join(deployDir, "_worker.js"),
  bundle: true,
  format: "esm",
  platform: "browser",
  target: "es2022",
  sourcemap: false,
  logLevel: "info",
});

await copyFile(join(root, "src", "index.html"), join(deployDir, "index.html"));
