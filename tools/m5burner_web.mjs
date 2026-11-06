#!/usr/bin/env node
// Publish firmware to the M5Burner web store (burner.m5stack.com), a separate
// platform from the desktop M5Burner catalog that tools/m5burner.mjs uploads to:
// its own login, firmware ids and listing.
//
//   node tools/m5burner_web.mjs login                  ask for e-mail and password, save the token
//   node tools/m5burner_web.mjs list                   firmware uploaded by this account
//   node tools/m5burner_web.mjs upload <version> <firmware.bin> <cover.png> [public]
//                                                      new firmware, private unless "public"
//   node tools/m5burner_web.mjs version <version> <firmware.bin> [public]
//                                                      add a version to the existing firmware (same name)
//   node tools/m5burner_web.mjs visibility <firmwareId> <versionId> public|private
//   node tools/m5burner_web.mjs describe <firmwareId>  update the description from tools/m5burner.json
//
// Name, description and links come from tools/m5burner.json; device and
// category ids are in WEB below. The token is the store's access token, kept
// in ~/.config/feiyuos/m5burner_web_token (mode 600) and never printed.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const API = 'https://burner.m5stack.com/api/v1';
const TOKEN_FILE = join(homedir(), '.config/feiyuos/m5burner_web_token');
const CONFIG = join(dirname(fileURLToPath(import.meta.url)), 'm5burner.json');
const WEB = {
  deviceIds: ['2093000000000000012'], // Cardputer ADV
  categoryIds: ['2190000000000000011'], // AI & Assistants
};

function die(message) {
  console.error(message);
  process.exit(1);
}

function token() {
  try {
    const value = readFileSync(TOKEN_FILE, 'utf8').trim();
    if (value) return value;
  } catch {}
  die('not logged in: run node tools/m5burner_web.mjs login');
}

// Responses are {code, msg, data} or {code, msg, rows, total}.
async function call(method, path, body, auth = true) {
  const headers = auth ? { Authorization: `Bearer ${token()}` } : {};
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
  const code = data.code ?? response.status;
  if (!response.ok || code >= 400) die(`${method} ${path}: HTTP ${response.status} ${data.msg ?? ''} ${JSON.stringify(data.data ?? data.raw ?? '').slice(0, 200)}`);
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
  const email = (await ask('M5Stack e-mail: ')).trim().toLowerCase();
  const password = await ask('password: ', true);
  if (!email || !password) die('e-mail and password are required');
  const data = await call('POST', '/auth/login', { email, password, rememberMe: true, deviceName: 'feiyuos-cli' }, false);
  const accessToken = data.data?.accessToken ?? data.accessToken;
  if (!accessToken) die('login failed: no access token in the response');
  mkdirSync(dirname(TOKEN_FILE), { recursive: true, mode: 0o700 });
  writeFileSync(TOKEN_FILE, accessToken, { mode: 0o600 });
  console.log(`logged in as ${email}, token saved to ${TOKEN_FILE}`);
}

async function list() {
  const data = await call('GET', '/users/me/firmwares?pageNum=1&pageSize=50&visibility=ALL');
  const rows = data.rows ?? data.data?.rows ?? data.data ?? [];
  if (!rows.length) console.log('no firmware uploaded yet');
  for (const f of rows) {
    const pick = ({ firmwareId, firmwareName, currentVersion, visibility, status, auditStatus, reviewStatus }) =>
      ({ firmwareId, firmwareName, currentVersion, visibility, status, auditStatus, reviewStatus });
    console.log(JSON.stringify(pick(f)));
  }
}

function fields(version, isPublic) {
  const config = JSON.parse(readFileSync(CONFIG, 'utf8'));
  const form = new FormData();
  form.append('versionName', version);
  form.append('sourceUrl', config.github);
  form.append('visibility', isPublic === 'public' ? 'PUBLIC' : 'PRIVATE');
  return { config, form };
}

async function upload(version, firmwarePath, coverPath, isPublic) {
  if (!version || !firmwarePath || !coverPath) die('usage: upload <version> <firmware.bin> <cover.png> [public]');
  const { config, form } = fields(version, isPublic);
  form.append('firmwareName', config.name);
  form.append('firmwareDescription', config.description);
  form.append('categoryIds', JSON.stringify(WEB.categoryIds));
  form.append('deviceIds', JSON.stringify(WEB.deviceIds));
  form.append('binFile', new Blob([readFileSync(firmwarePath)]), basename(firmwarePath));
  form.append('coverFile', new Blob([readFileSync(coverPath)], { type: 'image/png' }), basename(coverPath));
  const data = await call('POST', '/firmwares', form);
  console.log(`uploaded ${config.name} ${version}: ${JSON.stringify(data.data ?? data).slice(0, 300)}`);
}

// The web app adds a version by uploading under the existing firmware name.
async function version(name, firmwarePath, isPublic) {
  if (!name || !firmwarePath) die('usage: version <version> <firmware.bin> [public]');
  const { config, form } = fields(name, isPublic);
  form.append('firmwareName', config.name);
  form.append('categoryIds', JSON.stringify(WEB.categoryIds));
  form.append('deviceIds', JSON.stringify(WEB.deviceIds));
  form.append('binFile', new Blob([readFileSync(firmwarePath)]), basename(firmwarePath));
  const data = await call('POST', '/firmwares', form);
  const result = data.data ?? data;
  console.log(`${config.name} ${name}: ${JSON.stringify(result).slice(0, 300)}`);
  if (result.operationType === 'PROJECT_CREATED') console.log('WARNING: created a new firmware instead of a version');
}

async function visibility(firmwareId, versionId, state) {
  if (!firmwareId || !versionId || !['public', 'private'].includes(state)) die('usage: visibility <firmwareId> <versionId> public|private');
  const form = new FormData();
  form.append('visibility', state.toUpperCase());
  await call('PUT', `/firmwares/${encodeURIComponent(firmwareId)}/versions/${encodeURIComponent(versionId)}`, form);
  console.log(`${firmwareId} ${versionId}: ${state}`);
}

async function describe(firmwareId) {
  if (!firmwareId) die('usage: describe <firmwareId>');
  const config = JSON.parse(readFileSync(CONFIG, 'utf8'));
  const form = new FormData();
  form.append('firmwareDescription', config.description);
  const data = await call('PATCH', `/firmwares/${encodeURIComponent(firmwareId)}`, form);
  console.log(`${firmwareId}: description updated ${JSON.stringify(data.data ?? '').slice(0, 200)}`);
}

const [command, ...args] = process.argv.slice(2);
const commands = { login, list, upload, version, visibility, describe };
if (!commands[command]) die('usage: node tools/m5burner_web.mjs login|list|upload|version|visibility|describe (see the header of this file)');
await commands[command](...args);
