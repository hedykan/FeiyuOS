#!/usr/bin/env node
// Publish firmware to M5Burner, M5Stack's firmware store.
//
//   node tools/m5burner.mjs login                      ask for e-mail and password, save the token
//   node tools/m5burner.mjs list                       firmware uploaded by this account
//   node tools/m5burner.mjs upload <version> <firmware.bin> <cover.png>
//                                                      upload a version, kept private
//   node tools/m5burner.mjs version <version> <firmware.bin>
//                                                      add a version to the existing firmware (same
//                                                      name), kept private; also refreshes the description;
//                                                      its change log comes from docs/releases/<version>.md
//   node tools/m5burner.mjs share <fid> <version>      share code for burning a private version
//   node tools/m5burner.mjs publish <fid> <version> on|off
//
// Name, description and category come from tools/m5burner.json. The token is
// the m5_auth_token cookie of the M5Stack account, kept in
// ~/.config/feiyuos/m5burner_token (mode 600) and never printed. Everything
// goes over HTTPS.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { changeLog } from './release_notes.mjs';

const API = 'https://m5burner-api.m5stack.com';
const LOGIN = 'https://uiflow2.m5stack.com/api/v1/account/login';
const TOKEN_FILE = join(homedir(), '.config/feiyuos/m5burner_token');
const CONFIG = join(dirname(fileURLToPath(import.meta.url)), 'm5burner.json');

function die(message) {
  console.error(message);
  process.exit(1);
}

function token() {
  try {
    const value = readFileSync(TOKEN_FILE, 'utf8').trim();
    if (value) return value;
  } catch {}
  die('not logged in: run node tools/m5burner.mjs login');
}

function auth() {
  const t = token();
  return { Cookie: `m5_auth_token=${t}`, m5_auth_token: t };
}

async function call(method, path, body) {
  const headers = auth();
  if (body && !(body instanceof FormData)) {
    headers['content-type'] = 'application/json';
    body = JSON.stringify(body);
  }
  const response = await fetch(API + path, { method, headers, body });
  const text = await response.text();
  let data = {};
  try {
    data = text ? JSON.parse(text) : {};
  } catch {
    data = { raw: text.slice(0, 300) };
  }
  if (!response.ok) die(`${method} ${path}: HTTP ${response.status} ${JSON.stringify(data).slice(0, 300)}`);
  return data;
}

function ask(question, hidden) {
  return new Promise((resolve) => {
    const input = process.stdin;
    process.stdout.write(question);
    if (!hidden || !input.isTTY) {
      input.once('data', (chunk) => {
        input.pause();
        resolve(String(chunk).split(/\r?\n/)[0]);
      });
      input.resume();
      return;
    }
    let value = '';
    input.setRawMode(true);
    input.resume();
    input.setEncoding('utf8');
    const onData = (chunk) => {
      for (const ch of chunk) {
        if (ch === '\r' || ch === '\n') {
          input.setRawMode(false);
          input.pause();
          input.off('data', onData);
          process.stdout.write('\n');
          resolve(value);
          return;
        }
        if (ch === '\u0003') {
          input.setRawMode(false);
          process.stdout.write('\n');
          process.exit(130);
        }
        if (ch === '\u007f' || ch === '\b') value = value.slice(0, -1);
        else value += ch;
      }
    };
    input.on('data', onData);
  });
}

async function login() {
  const email = (await ask('M5Stack e-mail: ')).trim();
  const password = await ask('password: ', true);
  if (!email || !password) die('e-mail and password are required');
  const response = await fetch(LOGIN, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ email, password }),
  });
  const cookies = response.headers.getSetCookie?.() ?? [response.headers.get('set-cookie') ?? ''];
  const match = cookies.join(';').match(/m5_auth_token=([^;]+)/);
  if (!response.ok || !match) die(`login failed: HTTP ${response.status}`);
  mkdirSync(dirname(TOKEN_FILE), { recursive: true, mode: 0o700 });
  writeFileSync(TOKEN_FILE, match[1], { mode: 0o600 });
  console.log(`logged in as ${email}, token saved to ${TOKEN_FILE}`);
}

function versions(firmware) {
  // The API returns published as the string "True" or "False".
  return (firmware.versions ?? []).map(
    (v) => `${v.version}${String(v.published).toLowerCase() === 'true' ? ' (public)' : ' (private)'}`,
  );
}

async function list() {
  const data = await call('GET', '/api/admin/firmware');
  const items = Array.isArray(data) ? data : data.data ?? [];
  if (!items.length) console.log('no firmware uploaded yet');
  for (const f of items) console.log(`${f.fid}  ${f.name}  [${f.category}]  ${versions(f).join(', ')}`);
}

async function upload(version, firmwarePath, coverPath) {
  if (!version || !firmwarePath || !coverPath) die('usage: upload <version> <firmware.bin> <cover.png>');
  const config = JSON.parse(readFileSync(CONFIG, 'utf8'));
  const form = new FormData();
  for (const [key, value] of Object.entries({ ...config, version })) form.append(key, String(value));
  form.append('firmware', new Blob([readFileSync(firmwarePath)]), basename(firmwarePath));
  form.append('cover', new Blob([readFileSync(coverPath)]), basename(coverPath));
  const data = await call('POST', '/api/admin/firmware', form);
  const fid = data.fid ?? data.data?.fid;
  console.log(`uploaded ${config.name} ${version}, fid ${fid ?? '?'}, private`);
  console.log(`next: node tools/m5burner.mjs share ${fid} ${version}`);
}

// A new version goes through the upload endpoint with the existing
// firmware's name and author and no cover (as the M5Burner app does).
async function version(name, firmwarePath) {
  if (!name || !firmwarePath) die('usage: version <version> <firmware.bin>');
  const config = JSON.parse(readFileSync(CONFIG, 'utf8'));
  const data = await call('GET', '/api/admin/firmware');
  const items = Array.isArray(data) ? data : data.data ?? [];
  const existing = items.find((f) => f.name === config.name) ?? die(`no firmware named ${config.name}: use upload`);
  if ((existing.versions ?? []).some((v) => v.version === name)) die(`${name} is already uploaded`);
  let notes;
  try {
    notes = changeLog(name);
  } catch (error) {
    die(error.message);
  }
  const form = new FormData();
  const fields = { ...config, author: existing.author, version: name, cover: 'null', change_log: notes };
  for (const [key, value] of Object.entries(fields)) form.append(key, String(value));
  form.append('firmware', new Blob([readFileSync(firmwarePath)]), basename(firmwarePath));
  await call('POST', '/api/admin/firmware', form);
  const after = (await call('GET', '/api/admin/firmware')).filter?.((f) => f.name === config.name) ?? [];
  if (after.length !== 1) die(`expected one firmware named ${config.name}, found ${after.length}: check with list`);
  console.log(`${after[0].fid}: ${versions(after[0]).join(', ')}`);
  console.log(`next: node tools/m5burner.mjs share ${after[0].fid} ${name}`);
}

async function findVersion(fid, version) {
  const data = await call('GET', '/api/admin/firmware');
  const items = Array.isArray(data) ? data : data.data ?? [];
  const firmware = items.find((f) => f.fid === fid) ?? die(`no firmware ${fid} in this account`);
  return (firmware.versions ?? []).find((v) => v.version === version) ?? die(`no version ${version} of ${fid}`);
}

async function share(fid, version) {
  if (!fid || !version) die('usage: share <fid> <version>');
  const v = await findVersion(fid, version);
  const data = await call('POST', `/api/admin/firmware/share/${fid}/${encodeURIComponent(v.file)}`);
  console.log(JSON.stringify(data.data ?? data));
}

async function publish(fid, version, state) {
  if (!fid || !version || !['on', 'off'].includes(state)) die('usage: publish <fid> <version> on|off');
  // The endpoint takes the version's stored file name, not the version tag.
  const v = await findVersion(fid, version);
  await call('PUT', `/api/admin/firmware/${fid}/publish/${encodeURIComponent(v.file)}/${state === 'on' ? 1 : 0}`);
  console.log(`${fid} ${version}: ${state === 'on' ? 'public' : 'private'}`);
}

const [command, ...args] = process.argv.slice(2);
const commands = { login, list, upload, version, share, publish };
if (!commands[command]) die('usage: node tools/m5burner.mjs login|list|upload|version|share|publish (see the header of this file)');
await commands[command](...args);
