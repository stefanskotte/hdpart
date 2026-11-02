// Host-side reader for a hardfile written by HDPart. Validates every checksum,
// so a block the Amiga could not trust fails the test rather than parsing quietly.
import { readFileSync } from 'node:fs';

const NIL = 0xFFFFFFFF;
function block(d, n, id) {
  const b = d.subarray(n * 512, (n + 1) * 512);
  if (b.toString('latin1', 0, 4) !== id) throw new Error(`block ${n}: expected ${id}, found ${b.toString('latin1', 0, 4)}`);
  const longs = b.readUInt32BE(4);
  let sum = 0;
  for (let i = 0; i < longs; i++) sum = (sum + b.readUInt32BE(i * 4)) >>> 0;
  if (sum !== 0) throw new Error(`block ${n} (${id}): bad checksum`);
  return b;
}

export function readRdb(file) {
  const d = readFileSync(file);
  const r = block(d, 0, 'RDSK');
  const out = { cylBlocks: r.readUInt32BE(144), loCyl: r.readUInt32BE(136), hiCyl: r.readUInt32BE(140), parts: [], fs: [] };
  for (let n = r.readUInt32BE(28); n !== NIL;) {
    const p = block(d, n, 'PART');
    out.parts.push({ name: p.toString('latin1', 37, 37 + p[36]), low: p.readUInt32BE(128 + 36), high: p.readUInt32BE(128 + 40),
      dosType: p.readUInt32BE(128 + 64) });
    n = p.readUInt32BE(16);
  }
  for (let n = r.readUInt32BE(32); n !== NIL;) {
    const f = block(d, n, 'FSHD');
    const chunks = [];
    for (let s = f.readUInt32BE(72); s !== NIL;) {
      const l = block(d, s, 'LSEG');
      chunks.push(l.subarray(20, l.readUInt32BE(4) * 4));
      s = l.readUInt32BE(16);
    }
    out.fs.push({ dosType: f.readUInt32BE(32), patchFlags: f.readUInt32BE(40), seg: Buffer.concat(chunks) });
    n = f.readUInt32BE(16);
  }
  return out;
}

// First bytes of a partition, and whether a BSTR volume label is stored inside it.
export function partInfo(file, rdb, part) {
  const d = readFileSync(file);
  const start = part.low * rdb.cylBlocks * 512, end = (part.high + 1) * rdb.cylBlocks * 512;
  const body = d.subarray(start, end);
  return { magic: body.readUInt32BE(0), hasLabel: (s) => body.indexOf(Buffer.from([s.length, ...Buffer.from(s, 'latin1')])) >= 0 };
}
