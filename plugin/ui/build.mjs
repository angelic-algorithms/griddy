// Bundles the UI into three flat files in dist/: index.html, app.js, app.css.
// Flat on purpose: CMake embeds every file in dist/ into the plugin binary by file name.
import * as esbuild from "esbuild";
import { copyFile, mkdir } from "node:fs/promises";

const serve = process.argv.includes("--serve");

await mkdir("dist", { recursive: true });
await copyFile("src/index.html", "dist/index.html");

const options = {
  entryPoints: { app: "src/main.tsx" },
  bundle: true,
  outdir: "dist",
  format: "iife",
  // WKWebView on macOS 13 is Safari 16 era, so stay at that baseline.
  target: ["safari16", "chrome110"],
  minify: !serve,
  sourcemap: serve ? "inline" : false,
  logLevel: "info",
  // The fonts ride inside app.css, so the plugin needs no network and no extra files.
  loader: { ".woff2": "dataurl" },
};

if (serve) {
  // Build with -DGRIDDY_UI_DEV_SERVER=ON and the plugin loads this server instead of the
  // embedded files, so a save here shows up after reloading the plugin window. Opening
  // http://localhost:5173 in a normal browser runs the UI against mock data.
  const ctx = await esbuild.context(options);
  await ctx.watch();
  await ctx.serve({ servedir: "dist", port: 5173 });
} else {
  await esbuild.build(options);
}
