// Failure evidence that survives a re-run: out/target-failures.log (outside the
// per-Kickstart dirs, which every run wipes) plus a copy of the FAIL screenshot.
import { appendFileSync, copyFileSync, existsSync, mkdirSync } from 'node:fs';
import path from 'node:path';

export function recordFailure(root, { ks, name, error, lines, shot }) {
  try {
    const out = path.join(root, 'out');
    mkdirSync(out, { recursive: true });
    const stamp = new Date().toISOString();
    let shotCopy = '';
    if (shot && existsSync(shot)) {
      shotCopy = path.join(out, `target-failure-${stamp.replace(/[:.]/g, '-')}-${ks}.png`);
      copyFileSync(shot, shotCopy);
    }
    appendFileSync(path.join(out, 'target-failures.log'),
      [`=== ${stamp}  ${ks}`, `step: ${name}`, `error: ${error}`, ...lines,
       `screenshot: ${shotCopy || '(none)'}`, ''].join('\n') + '\n');
  } catch { /* evidence is best effort */ }
}
