#!/usr/bin/env node
// Targets for t-code, in JavaScript (Node.js 18 or newer, its standard library only).
//
//   run    the project's Make, just, Task and npm targets in t-code's picker; the chosen one runs
//          in a "Targets" terminal tab, and its compiler errors (file:line:col: error: ...) become
//          diagnostics when it ends
//   again  runs the last target again
//
// It speaks tapi_1 over $TCODE_SOCKET (t-code's docs/plugins.md).
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');

// Talking to t-code ------------------------------------------------------------------------------

class TcodeError extends Error {
  constructor(code, message) {
    super(`${code}: ${message}`);
    this.code = code;
  }
}

class Tcode {
  // A tapi_1 connection, said hello to with the plugin's token.
  static async connect(socketPath = process.env.TCODE_SOCKET, token = process.env.TCODE_PLUGIN_TOKEN || '') {
    const t = new Tcode(net.createConnection(socketPath));
    await new Promise((resolve, reject) => {
      t.sock.once('connect', resolve);
      t.sock.once('error', reject);
    });
    await t.request('hello', `tapi_1 ${token}`.trim());
    return t;
  }

  constructor(sock) {
    this.sock = sock;
    this.buffer = Buffer.alloc(0);
    this.waiting = []; // {until, resolve, reject}: readers in the order the answers come
    sock.on('data', (data) => {
      this.buffer = Buffer.concat([this.buffer, data]);
      this.feed();
    });
    const closed = () => {
      for (const w of this.waiting.splice(0)) w.reject(new TcodeError('closed', 't-code closed the connection'));
    };
    sock.on('close', closed);
    sock.on('error', closed);
  }

  feed() {
    while (this.waiting.length) {
      const end = this.buffer.indexOf(this.waiting[0].until);
      if (end < 0) return;
      const part = this.buffer.subarray(0, end).toString('utf8');
      this.buffer = this.buffer.subarray(end + 1);
      this.waiting.shift().resolve(part);
    }
  }

  read(until) {
    return new Promise((resolve, reject) => {
      this.waiting.push({ until, resolve, reject });
      this.feed();
    });
  }

  // One request, ended by a NUL (an argument may hold line breaks); the value after "ok".
  async request(command, argument = '') {
    const answer = this.read(0x0a);
    this.sock.write(`${command} ${argument}\0`);
    const line = await answer;
    if (line === 'ok') return '';
    if (line.startsWith('ok ')) return line.slice(3);
    const rest = line.startsWith('error ') ? line.slice(6) : line;
    const space = rest.indexOf(' ');
    throw new TcodeError(space < 0 ? rest : rest.slice(0, space), space < 0 ? '' : rest.slice(space + 1));
  }

  json(command, value) {
    return this.request(command, JSON.stringify(value));
  }

  async say(text) {
    await this.request('say', text).catch(() => {});
  }

  close() {
    this.sock.end();
  }
}

// Finding targets --------------------------------------------------------------------------------

function readFile(file) {
  try {
    return fs.readFileSync(file, 'utf8');
  } catch {
    return null;
  }
}

// The comment above line i ("# Builds it"), or the "## text" after the target on its line.
function docAt(lines, i, sameLine) {
  const inline = /##\s*(.+)$/.exec(sameLine);
  if (inline) return inline[1].trim();
  const above = /^\s*#+\s?(.*)$/.exec(lines[i - 1] || '');
  return above ? above[1].trim() : '';
}

function makeTargets(text) {
  const lines = text.split('\n');
  const found = new Map();
  lines.forEach((line, i) => {
    const phony = /^\.PHONY\s*:\s*(.*)$/.exec(line);
    if (phony) {
      for (const name of phony[1].split(/\s+/).filter(Boolean)) if (!found.has(name)) found.set(name, '');
      return;
    }
    const rule = /^([A-Za-z0-9_][A-Za-z0-9_.\-/ ]*?)\s*:(?![=:])/.exec(line);
    if (!rule) return;
    for (const name of rule[1].split(/\s+/)) {
      if (!name || name.startsWith('.') || name.includes('%') || name.includes('$')) continue;
      const doc = docAt(lines, i, line);
      if (!found.get(name)) found.set(name, doc);
    }
  });
  return [...found].map(([name, doc]) => ({ name, doc }));
}

function justTargets(text) {
  const lines = text.split('\n');
  const found = [];
  lines.forEach((line, i) => {
    const recipe = /^@?([A-Za-z0-9_][A-Za-z0-9_-]*)(\s+[^:=]*)?:(?!=)/.exec(line);
    if (recipe && !/^(set|alias|export|import|mod)\b/.test(line)) found.push({ name: recipe[1], doc: docAt(lines, i, '') });
  });
  return found;
}

// The keys under "tasks:" in a Taskfile, two spaces in, with their "desc:".
function taskTargets(text) {
  const found = [];
  let inTasks = false;
  for (const line of text.split('\n')) {
    if (/^tasks:\s*$/.test(line)) {
      inTasks = true;
      continue;
    }
    if (/^\S/.test(line)) inTasks = false;
    if (!inTasks) continue;
    const task = /^  ([A-Za-z0-9_:.-]+):\s*(\S.*)?$/.exec(line);
    if (task) {
      found.push({ name: task[1], doc: '' });
      continue;
    }
    const desc = /^    desc:\s*["']?(.*?)["']?\s*$/.exec(line);
    if (desc && found.length) found[found.length - 1].doc = desc[1];
  }
  return found;
}

function npmTargets(text) {
  try {
    const scripts = JSON.parse(text).scripts || {};
    return Object.entries(scripts).map(([name, command]) => ({ name, doc: String(command) }));
  } catch {
    return [];
  }
}

const SOURCES = [
  { files: ['GNUmakefile', 'Makefile', 'makefile'], tool: 'make', parse: makeTargets },
  { files: ['justfile', 'Justfile', '.justfile'], tool: 'just', parse: justTargets },
  { files: ['Taskfile.yml', 'Taskfile.yaml', 'taskfile.yml'], tool: 'task', parse: taskTargets },
  { files: ['package.json'], tool: 'npm run', parse: npmTargets },
];

// Every target of the project's root: {tool, name, doc, file}.
function findTargets(root) {
  const all = [];
  for (const source of SOURCES) {
    for (const name of source.files) {
      const text = readFile(path.join(root, name));
      if (text === null) continue;
      for (const t of source.parse(text)) all.push({ tool: source.tool, name: t.name, doc: t.doc, file: name });
      break;
    }
  }
  return all;
}

// Running one ------------------------------------------------------------------------------------

function quote(word) {
  return /^[A-Za-z0-9_.,:/=+-]+$/.test(word) ? word : `'${word.replace(/'/g, `'\\''`)}'`;
}

function runDir(root) {
  const base = process.env.XDG_RUNTIME_DIR || os.tmpdir();
  const hash = crypto.createHash('sha256').update(root).digest('hex').slice(0, 12);
  return path.join(base, 't-code-targets', hash);
}

// The script the terminal tab runs: the target, its output kept in log, its status in status (also
// when the tab is closed under it).
function scriptText(root, command, dir) {
  const log = quote(path.join(dir, 'log'));
  const status = quote(path.join(dir, 'status'));
  return [
    '#!/bin/sh',
    `trap 'echo 129 > ${status}; exit 129' HUP TERM`,
    `cd ${quote(root)} || { echo 1 > ${status}; exit 1; }`,
    `printf '\\033[1m$ %s\\033[0m\\n' ${quote(command)}`,
    `{ ( ${command} ); echo $? > ${status}.part; } 2>&1 | tee ${log}`, // its own subshell: an exit in it ends only it
    `mv -f ${status}.part ${status}`,
    `code=$(cat ${status})`,
    `[ "$code" = 0 ] && printf '\\033[32mdone\\033[0m\\n' || printf '\\033[31mfailed (%s)\\033[0m\\n' "$code"`,
    '',
  ].join('\n');
}

// Compiler messages: "file:line:col: error: text" (GCC, Clang, Rust's "--> file:line:col" aside),
// "file:line: error: text"; make's "Entering directory" moves where relative paths start.
function parseProblems(log, root) {
  const found = new Map();
  let dir = root;
  for (const line of log.split('\n')) {
    const entering = /^\S*make(?:\[\d+\])?: Entering directory [`'"](.+)['"]$/.exec(line);
    if (entering) {
      dir = entering[1];
      continue;
    }
    // no quotes in the file part: make echoes command lines, printf "a.c:1:2: error: ..." among them
    const m = /^([^\s:"'][^:"']*):(\d+):(?:(\d+):)? (fatal error|error|warning|note): (.*)$/.exec(line);
    if (!m) continue;
    const file = path.resolve(dir, m[1]);
    const severity = m[4] === 'warning' ? 'warning' : m[4] === 'note' ? 'info' : 'error';
    const items = found.get(file) || [];
    items.push({ range: [Number(m[2]), Number(m[3] || 1)], severity, message: m[5], source: 'build' });
    found.set(file, items);
  }
  return found;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Opens (or reuses) the Targets tab, runs command in it, and once it ends turns its output into
// diagnostics. The connection is closed while it runs: t-code is not kept waiting on it.
async function runTarget(t, root, target) {
  const command = `${target.tool} ${quote(target.name)}`;
  const dir = runDir(root);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, 'last.json'), JSON.stringify(target));
  for (const name of ['status', 'status.part', 'log']) fs.rmSync(path.join(dir, name), { force: true });
  const script = path.join(dir, 'run.sh');
  fs.writeFileSync(script, scriptText(root, command, dir), { mode: 0o755 });
  await t.request('diagnostics-clear');
  let tab = readFile(path.join(dir, 'tab'));
  try {
    if (!tab) throw new TcodeError('bad-argument', 'no tab yet');
    await t.request('terminal-send', `${tab} sh ${quote(script)}`);
  } catch (e) {
    if (e.code !== 'bad-argument') throw e;
    tab = await t.request('terminal-open', `Targets -- sh -c ${quote(`sh ${quote(script)}; exec "\${SHELL:-sh}"`)}`);
    fs.writeFileSync(path.join(dir, 'tab'), tab);
  }
  t.close();
  const statusFile = path.join(dir, 'status');
  for (let waited = 0; !fs.existsSync(statusFile) && waited < 6 * 3600 * 1000; waited += 500) await sleep(500);
  const code = (readFile(statusFile) || '').trim();
  const problems = parseProblems(readFile(path.join(dir, 'log')) || '', root);
  const after = await Tcode.connect();
  for (const [file, items] of problems) await after.json('diagnostics-set', { path: file, items });
  const count = [...problems.values()].reduce((n, items) => n + items.filter((i) => i.severity === 'error').length, 0);
  if (code === '0') await after.say(`Targets: ${command} done`);
  else if (code !== '129') await after.say(`Targets: ${command} failed${count ? `, ${count} error${count === 1 ? '' : 's'} in Problems` : ''}`);
  after.close();
}

async function project(t) {
  return process.env.TCODE_PROJECT_ROOT || (await t.request('get-project'));
}

async function pickAndRun() {
  const t = await Tcode.connect();
  const root = await project(t);
  const targets = findTargets(root);
  if (!targets.length) {
    await t.say('Targets: no Makefile, justfile, Taskfile or package.json scripts here');
    return t.close();
  }
  let answer;
  try {
    answer = await t.json('pick', {
      title: 'Run a target',
      items: targets.map((x) => ({ label: `${x.tool} ${x.name}`, detail: x.doc || x.file })),
    });
  } catch (e) {
    if (e.code === 'canceled') return t.close();
    throw e;
  }
  await runTarget(t, root, targets[Number(answer.split(' ')[0])]);
}

async function again() {
  const t = await Tcode.connect();
  const root = await project(t);
  let last;
  try {
    last = JSON.parse(readFile(path.join(runDir(root), 'last.json')));
  } catch {
    last = null;
  }
  if (!last) {
    t.close();
    return pickAndRun();
  }
  await runTarget(t, root, last);
}

module.exports = { Tcode, makeTargets, justTargets, taskTargets, npmTargets, findTargets, parseProblems, scriptText, quote };

if (require.main === module) {
  const command = process.argv[2] || '';
  const run = { run: pickAndRun, again }[command];
  if (!run) {
    console.error(`targets: unknown command ${JSON.stringify(command)}`);
    process.exit(1);
  }
  run().catch((e) => {
    if (e.code === 'closed') return;
    console.error(`targets: ${command}: ${e.message}`);
    process.exit(1);
  });
}
