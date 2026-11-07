// "What's new" of a release, for the stores' per-version change log.
//
// docs/releases/<version>.md is the GitHub release text (docs/RELEASE.zh-CN.md
// has the template).  The part before "## 下载哪个文件" (the one-line summary
// and the new / fixed lists) becomes plain text: no Markdown marks, links as
// their words, the "完整说明见" pointer dropped.
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'docs', 'releases');

export function notesPath(version) {
  return join(DIR, `${version}.md`);
}

export function changeLog(version) {
  const path = notesPath(version);
  if (!existsSync(path)) {
    throw new Error(`${path} is missing: write the release notes first (docs/RELEASE.zh-CN.md)`);
  }
  const text = readFileSync(path, 'utf8').split(/^## 下载哪个文件/m)[0];
  const lines = text
    .split('\n')
    .filter((line) => !line.startsWith('完整说明见'))
    .map((line) =>
      line
        .replace(/^##\s+(.*)$/, '【$1】')
        .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
        .replace(/\*\*|`/g, '')
        .replace(/^- /, '· ')
        .trimEnd(),
    );
  const out = lines.join('\n').replace(/\n{3,}/g, '\n\n').trim();
  if (!out) throw new Error(`${path}: nothing before "## 下载哪个文件"`);
  return out;
}

// node tools/release_notes.mjs vX: print what the stores will show.
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    console.log(changeLog(process.argv[2] ?? ''));
  } catch (error) {
    console.error(error.message);
    process.exit(1);
  }
}
