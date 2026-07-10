// Usage (from the repository root, after building):
//   node src/wasm/node_arithmetic_test.mjs [path/to/zelph.mjs]

import { pathToFileURL } from "node:url";

const modulePath = process.argv[2] ?? "build-wasm/bin/zelph.mjs";
const { default: createZelphModule } = await import(
  pathToFileURL(modulePath).href
);

const Module = await createZelphModule();
const processLine = Module.cwrap("zelph_process", null, ["string"]);

for (const line of [".import binary-arithmetic", "(&12 * &34) = X"]) {
  console.log(`zelph> ${line}`);
  processLine(line);
}

console.log("-- done --");
