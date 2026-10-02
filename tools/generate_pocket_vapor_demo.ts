// Run with Bun after cloning pocket-nexus/pocket-vapor (experiment/standalone)
// beside this repository. Only the generated C is linked into SkyOS.
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { compileVaporApp, VAPOR_TARGETS } from "../../pocket-vapor/vapor/compiler/compile.ts";

const root = resolve(fileURLToPath(new URL("..", import.meta.url)));
const dir = resolve(root, "System/applications/user/pocket_vapor_demo");
const entry = resolve(dir, "Counter.tsx");

// Reuse the upstream 1-bit glyph / RGB565 style emitter, but fit this LCD's
// 256x127 pixels as 32 columns by 15 rows of 8x8 cells. The device adapter
// converts the emitted colors to its 8-bit grayscale framebuffer.
VAPOR_TARGETS.esp32.width = 32;
VAPOR_TARGETS.esp32.height = 15;
const app = compileVaporApp(entry, readFileSync(entry, "utf8"), "SKYOS DEMO", "esp32", { strict: true });
writeFileSync(resolve(dir, "gen_app.c"), app.c);
writeFileSync(resolve(dir, "profile.h"),
  `/* Generated with gen_app.c; shared by the native adapter and core. */\n` +
  `#define VP_GRID_W 32\n#define VP_GRID_H 15\n` +
  `#define SKYOS_VP_STYLE_COUNT ${app.styles.pairs.length}\n`);
writeFileSync(resolve(dir, "memory_plan.txt"), `${app.graph}\n\n${app.plan}\n`);
console.log(app.graph);
console.log(app.plan);
