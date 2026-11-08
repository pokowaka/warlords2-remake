// The scenario generator's worlds as files, for checking the ports against
// each other byte for byte (cpp/test/scengen_compare.sh):
//
//     node web/test/scengen_dump.js DIR SHAPE FIRST COUNT
//
// writes, for the seeds FIRST .. FIRST+COUNT-1, DIR/<shape>-<seed>/RANDOM.<ext>
// for every file the generator makes (SCN MAP RD SGN CTY SPC) and TITLE.TXT,
// the world's name and description a line each -- what
// `w2scengen --dump DIR --shape SHAPE --seed FIRST --count COUNT` writes
// (cpp/test/scengen.cpp). The data files are the ones web/test/run.js reads.
import * as fs from "node:fs";
import * as path from "node:path";
import { loadData } from "./node.js";
import { Rng } from "../src/warlords/rng.js";
import * as randommap from "../src/warlords/randommap.js";
import * as scengen from "../src/warlords/scengen.js";

const [dir, shapeArg, firstArg, countArg] = process.argv.slice(2);
const shape = scengen.parseShape(shapeArg || "");
if (!dir || !shape || !firstArg) {
  console.error("usage: node web/test/scengen_dump.js DIR continents|pangaea|islands FIRST [COUNT]");
  process.exit(2);
}
const first = parseInt(firstArg, 10), count = Math.max(1, parseInt(countArg || "1", 10));

loadData({ images: false });
for (let k = 0; k < count; k++) {
  const seed = first + k;
  const files = randommap.generateNow({
    dataDir: "", rng: new Rng(seed), scenario: true, world: scengen.settings({ shape }),
  });
  const out = path.join(dir, `${scengen.shapeName(shape)}-${seed}`);
  fs.mkdirSync(out, { recursive: true });
  for (const ext of Object.keys(files)) fs.writeFileSync(path.join(out, `${randommap.DIR}.${ext}`), files[ext]);
  const t = scengen.title(shape, seed);
  fs.writeFileSync(path.join(out, "TITLE.TXT"), Buffer.from(t.name + "\n" + t.text + "\n", "latin1"));
}
