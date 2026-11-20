// The plugin's pieces: node --test (make test runs it when node is installed).
'use strict';

const assert = require('node:assert/strict');
const { execFile, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const targets = require('./targets.js');

const MAKEFILE = `CC ?= cc
.PHONY: all clean test
# Builds everything
all: app
app: main.o util.o ## the program
\t$(CC) -o $@ $^
%.o: %.c
\t$(CC) -c $<
clean:
\trm -f app *.o
VAR := value
$(OUT): x
`;

test('make targets: rules, .PHONY, docs; no patterns, variables or assignments', () => {
  assert.deepEqual(targets.makeTargets(MAKEFILE), [
    { name: 'all', doc: 'Builds everything' },
    { name: 'clean', doc: '' },
    { name: 'test', doc: '' },
    { name: 'app', doc: 'the program' },
  ]);
});

test('just, Task and npm targets', () => {
  assert.deepEqual(targets.justTargets('set shell := ["bash", "-c"]\n# Run the tests\ntest *args:\n  cargo test {{args}}\n@build:\n  cargo build\n'), [
    { name: 'test', doc: 'Run the tests' },
    { name: 'build', doc: '' },
  ]);
  assert.deepEqual(targets.taskTargets('version: "3"\ntasks:\n  build:\n    desc: "Build it"\n    cmds:\n      - go build\n  lint:\n    cmds: [golangci-lint run]\nvars:\n  X: 1\n'), [
    { name: 'build', doc: 'Build it' },
    { name: 'lint', doc: '' },
  ]);
  assert.deepEqual(targets.npmTargets('{"scripts": {"dev": "vite", "test": "vitest"}}'), [
    { name: 'dev', doc: 'vite' },
    { name: 'test', doc: 'vitest' },
  ]);
  assert.deepEqual(targets.npmTargets('not json'), []);
});

test('compiler messages become diagnostics, relative to make -C folders', () => {
  const log = [
    'cc -c main.c',
    'main.c:3:5: error: unknown type name ‘foo’',
    'make[1]: Entering directory \'/p/lib\'',
    'util.c:10:1: warning: unused variable',
    'util.c:12: error: old style',
    'In file included from x.h:1,',
    'random text: not a message',
  ].join('\n');
  const found = targets.parseProblems(log, '/p');
  assert.deepEqual(found.get('/p/main.c'), [{ range: [3, 5], severity: 'error', message: 'unknown type name ‘foo’', source: 'build' }]);
  assert.deepEqual(found.get('/p/lib/util.c').map((d) => [d.range, d.severity]), [[[10, 1], 'warning'], [[12, 1], 'error']]);
  assert.equal(found.size, 2);
});

test('the run script keeps the output and the exit status', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 't-code-targets-'));
  fs.writeFileSync(path.join(dir, 'run.sh'), targets.scriptText(dir, "printf 'a.c:1:2: error: nope\\n'; exit 3", dir));
  execFileSync('sh', [path.join(dir, 'run.sh')], { stdio: 'ignore' });
  assert.equal(fs.readFileSync(path.join(dir, 'status'), 'utf8').trim(), '3');
  assert.match(fs.readFileSync(path.join(dir, 'log'), 'utf8'), /a\.c:1:2: error: nope/);
  fs.rmSync(dir, { recursive: true });
});

test('quote leaves plain words alone', () => {
  assert.equal(targets.quote('build'), 'build');
  assert.equal(targets.quote("it's"), `'it'\\''s'`);
});

// A t-code answering on a real socket: hello, then a pick of the second target, then the tab.
test('run: pick, then the target in a terminal tab, then its errors as diagnostics', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 't-code-targets-'));
  fs.writeFileSync(path.join(dir, 'Makefile'), 'ok:\n\ttrue\nbad:\n\tprintf "x.c:2:3: error: broken\\n"; false\n');
  const sock = path.join(dir, 't-code.sock');
  const requests = [];
  const server = net.createServer((conn) => {
    let pending = '';
    conn.on('data', (data) => {
      pending += data.toString();
      let end;
      while ((end = pending.indexOf('\0')) >= 0) {
        const request = pending.slice(0, end);
        pending = pending.slice(end + 1);
        requests.push(request);
        const command = request.split(' ')[0];
        if (command === 'hello') conn.write('ok tapi_1 t-code test\n');
        else if (command === 'pick') conn.write('ok 1 make bad\n');
        else if (command === 'terminal-send') conn.write('error bad-argument no such tab\n');
        else if (command === 'terminal-open') {
          // the tab runs the script: here, a child shell does
          const script = /sh -c '?sh (\S+?)[;']/.exec(request)[1].replace(/'/g, '');
          execFileSync('sh', [script], { stdio: 'ignore' });
          conn.write('ok 7\n');
        } else conn.write('ok\n');
      }
    });
  });
  await new Promise((resolve) => server.listen(sock, resolve));
  assert.deepEqual(targets.findTargets(dir).map((x) => x.name), ['ok', 'bad']);
  // the whole command, as t-code starts it (asynchronously: this process is the fake t-code)
  await new Promise((resolve, reject) =>
    execFile(process.execPath, [path.join(__dirname, 'targets.js'), 'run'], {
      env: { ...process.env, TCODE_SOCKET: sock, TCODE_PLUGIN_TOKEN: 'secret', TCODE_PROJECT_ROOT: dir, XDG_RUNTIME_DIR: dir },
    }, (error, stdout, stderr) => (error ? reject(new Error(stderr || error.message)) : resolve())),
  );
  server.close();
  assert.equal(requests[0], 'hello tapi_1 secret');
  assert.match(requests[1], /^pick \{"title":"Run a target","items":\[\{"label":"make ok","detail":"Makefile"\},\{"label":"make bad"/);
  assert.equal(requests[2], 'diagnostics-clear ');
  // no tab yet: straight to terminal-open; then a second connection for the results
  assert.match(requests[3], /^terminal-open Targets -- sh -c 'sh \S+run\.sh; exec "\$\{SHELL:-sh\}"'$/);
  assert.equal(requests[4], 'hello tapi_1 secret');
  assert.equal(requests[5], `diagnostics-set {"path":"${path.join(dir, 'x.c')}","items":[{"range":[2,3],"severity":"error","message":"broken","source":"build"}]}`);
  assert.equal(requests[6], 'say Targets: make bad failed, 1 error in Problems');
  assert.equal(requests.length, 7);
  fs.rmSync(dir, { recursive: true });
});
