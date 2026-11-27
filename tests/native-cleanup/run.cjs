'use strict';

const fs = require('node:fs');
const path = require('node:path');
const child = require('node:child_process');
const snapshot = require('./snapshot.cjs');
const nativeFixtures = require('./wsl-fixtures.cjs');
const { root, digest, capacity } = snapshot;
const ownedSources = ['native/core/diagnostics/junk.hpp', 'native/core/diagnostics/junk.cpp',
  'tests/native-cleanup/cleanup_tests.cpp', 'tests/native-cleanup/CMakeLists.txt',
  'tests/native-cleanup/run.cjs', 'tests/native-cleanup/snapshot.cjs', 'tests/native-cleanup/wsl-fixtures.cjs'];
const productionInputs = ['native/core/config/atomic_file.cpp', 'native/core/config/path_guard.cpp',
  'native/core/config/detail.cpp', 'native/core/config/crypto.cpp', 'native/core/tasks/secure_fs.cpp',
  'native/core/network/hash.cpp', 'native/core/network/control.cpp'];
const modes = new Set(['--build', '--test', '--syntax', '--tools']);
const windowsToolsPrefix = 'D:/workon/nickel-native-tools/install/windows-x64-static';

function options() {
  const args = process.argv.slice(2);
  const mode = modes.has(args[0]) ? args.shift() : '--build';
  let commit = process.env.NICKEL_CLEANUP_SOURCE_COMMIT;
  let distribution;
  let tmpfs = false;
  while (args.length) {
    const flag = args.shift();
    if (flag === '--commit' && !commit) commit = args.shift();
    else if (flag === '--wsl' && !distribution) distribution = args.shift();
    else if (flag === '--wsl-tmpfs' && !tmpfs) tmpfs = true;
    else throw new Error('Usage: node tests/native-cleanup/run.cjs [--build|--test|--syntax|--tools] --commit <full-git-commit> [--wsl <distribution> [--wsl-tmpfs]]');
  }
  if (!commit || !/^[0-9a-f]{40}$/.test(commit)) throw new Error('An exact immutable dependency Git commit is required');
  if (distribution && (process.platform !== 'win32' || !/^[A-Za-z0-9._-]+$/.test(distribution))) throw new Error('Invalid WSL distribution');
  if (tmpfs && !distribution) throw new Error('Native memory fixtures require an explicit WSL distribution');
  return { mode, commit, distribution, tmpfs };
}

function hashes() {
  return Object.fromEntries(ownedSources.map(relative => [relative, digest(fs.readFileSync(path.join(root, relative)))]));
}

function execute(command, args, environment, evidence, allowFailure = false) {
  capacity();
  const start = new Date().toISOString();
  const result = child.spawnSync(command, args, { cwd: root, env: environment, shell: false,
    encoding: 'utf8', maxBuffer: 16 * 1024 * 1024, timeout: 10 * 60 * 1000, windowsHide: true });
  const item = { command, args, cwd: root, start, end: new Date().toISOString(), status: result.status,
    signal: result.signal, error: result.error ? String(result.error) : undefined,
    stdout: result.stdout || '', stderr: result.stderr || '' };
  evidence.commands.push(item);
  process.stdout.write(item.stdout);
  process.stderr.write(item.stderr);
  if (result.error || (!allowFailure && result.status !== 0)) throw new Error(`Command failed: ${command}, status=${result.status}, error=${result.error || 'none'}`);
  return item;
}

function compilation(build, dependency, hostPath, hostRoot) {
  const entries = JSON.parse(fs.readFileSync(path.join(build, 'compile_commands.json'), 'utf8'));
  const expected = new Map(productionInputs.map(relative => [hostPath(path.join(dependency.shared, relative)), dependency.files[relative].sha256]));
  for (const relative of ['native/core/diagnostics/junk.cpp', 'tests/native-cleanup/cleanup_tests.cpp']) {
    expected.set(hostPath(path.join(root, relative)), digest(fs.readFileSync(path.join(root, relative))));
  }
  const seen = new Set();
  const inputs = [];
  const normalize = input => process.platform === 'win32' && !hostRoot ? path.resolve(input).toLowerCase() : input;
  const keys = new Map([...expected].map(([file, sha256]) => [normalize(file), sha256]));
  for (const entry of entries) {
    const file = normalize(entry.file);
    const command = entry.command || (entry.arguments || []).join(' ');
    if (!keys.has(file) || seen.has(file) || !command.includes('NICKEL_TRANSPORT_TEST_HOOKS')) throw new Error(`Unexpected, duplicate, or non-controlled compiler input: ${entry.file}`);
    seen.add(file);
    inputs.push({ file: entry.file, sha256: keys.get(file), command });
  }
  if (seen.size !== keys.size) throw new Error('Incomplete production cleanup compiler closure');
  return { commandsSha256: digest(fs.readFileSync(path.join(build, 'compile_commands.json'))),
    translationUnits: inputs.length, junkCompilationCount: inputs.filter(item => /[/\\]junk\.cpp$/.test(item.file)).length, inputs };
}

let evidence;
let logDirectory;
let dependency;
let fixtureLease;
let fixtureTool;
try {
  const configuration = options();
  const available = capacity();
  dependency = snapshot.capture(configuration.commit);
  const sourceHashes = hashes();
  const signature = digest(Buffer.from(JSON.stringify({ sourceHashes, dependency: dependency.manifestSha256 })));
  const platform = configuration.distribution ? 'linux' : process.platform;
  const label = configuration.distribution ? `wsl-${configuration.distribution}` : platform;
  const work = path.join(root, '.snow/palladium-cleanup/work', signature.slice(0, 20), label);
  const temp = path.join(work, 'temp');
  const fixtures = path.join(work, 'fixtures');
  const build = path.join(work, 'build');
  logDirectory = path.join(root, '.snow/palladium-cleanup/evidence');
  for (const directory of [temp, fixtures, logDirectory]) fs.mkdirSync(directory, { recursive: true });
  const environment = { ...process.env, TEMP: temp, TMP: temp, TMPDIR: temp,
    PYTHONDONTWRITEBYTECODE: '1', CMAKE_BUILD_PARALLEL_LEVEL: '1', CTEST_PARALLEL_LEVEL: '1' };
  evidence = { format: 'palladium-cleanup-local-run-v4', product: 'Palladium', mode: configuration.mode,
    started: new Date().toISOString(), platform, architecture: process.arch, wslDistribution: configuration.distribution,
    sourceSignature: signature, freeBytesBefore: available, temp, fixtures, build,
    dependencies: { sourceCommit: dependency.sourceCommit, manifestPath: path.relative(root, path.join(dependency.shared, 'snapshot.json')),
      manifestSha256: dependency.manifestSha256, filesMatched: dependency.filesMatched, files: dependency.files },
    sources: sourceHashes, commands: [], scope: 'Standalone production cleanup; not AppServices, product entry, visual, or release acceptance' };
  const capturedSource = path.join(work, 'source');
  for (const relative of ownedSources) {
    const output = path.join(capturedSource, relative);
    if (!fs.existsSync(output)) {
      fs.mkdirSync(path.dirname(output), { recursive: true });
      fs.copyFileSync(path.join(root, relative), output, fs.constants.COPYFILE_EXCL);
    }
    if (digest(fs.readFileSync(output)) !== sourceHashes[relative]) throw new Error('Captured source binding changed');
  }
  evidence.capturedSource = capturedSource;
  let hostRoot;
  if (configuration.distribution) {
    const windowsRoot = root.split(path.sep).join('/');
    const converted = execute('wsl.exe', ['-d', configuration.distribution, '--exec', 'wslpath', '-a', windowsRoot], environment, evidence).stdout.trim();
    if (!converted.startsWith('/')) throw new Error('WSL did not resolve the exact worktree path');
    hostRoot = converted;
  }
  const hostPath = file => {
    if (!hostRoot) return file;
    const relative = path.relative(root, path.resolve(file));
    if (relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) throw new Error('Host input escapes owned worktree');
    return path.posix.join(hostRoot, relative.split(path.sep).join('/'));
  };
  const tool = (name, args, allowFailure = false) => {
    if (configuration.distribution) return execute('wsl.exe', ['-d', configuration.distribution, '--cd', hostRoot, '--exec', 'env',
      `TMPDIR=${hostPath(temp)}`, `TEMP=${hostPath(temp)}`, `TMP=${hostPath(temp)}`, 'PYTHONDONTWRITEBYTECODE=1',
      'CMAKE_BUILD_PARALLEL_LEVEL=1', 'CTEST_PARALLEL_LEVEL=1', name, ...args], environment, evidence, allowFailure);
    if (platform === 'win32') return execute('python', [path.join(dependency.shared, 'scripts/native/msvc.py'), '--', name, ...args], environment, evidence, allowFailure);
    return execute(name, args, environment, evidence, allowFailure);
  };
  fixtureTool = tool;
  if (configuration.distribution) evidence.host = tool('uname', ['-a']).stdout.trim();
  if (configuration.mode === '--tools') {
    tool('cmake', ['--version']);
    tool('ninja', ['--version']);
    evidence.scope = 'Tool probe only; no runtime assertions';
  } else if (configuration.mode === '--syntax') {
    const sources = ['native/core/diagnostics/junk.cpp', 'tests/native-cleanup/cleanup_tests.cpp'];
    const includes = [hostPath(path.join(dependency.shared, 'native')), hostPath(path.join(root, 'native'))];
    if (platform === 'win32') {
      if (!fs.statSync(path.join(windowsToolsPrefix, 'include/nlohmann/json.hpp')).isFile()) {
        throw new Error('The configured read-only tool prefix is missing nlohmann/json.hpp');
      }
      for (const relative of sources) tool('cl', ['/nologo', '/std:c++20', '/W4', '/WX', '/utf-8', '/permissive-', '/Zs', '/EHsc',
        '/DNICKEL_TRANSPORT_TEST_HOOKS', ...includes.flatMap(directory => ['/I', directory]),
        `/external:I${path.join(windowsToolsPrefix, 'include')}`, '/external:W0', hostPath(path.join(root, relative))]);
    } else {
      for (const relative of sources) tool('c++', ['-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only',
        '-DNICKEL_TRANSPORT_TEST_HOOKS', ...includes.flatMap(directory => ['-I', directory]), hostPath(path.join(root, relative))]);
    }
    evidence.scope = 'Cleanup and regression syntax only; no runtime assertions';
  } else {
    if (configuration.mode === '--build') {
      const args = ['-S', hostPath(path.join(root, 'tests/native-cleanup')), '-B', hostPath(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        `-DNICKEL_CLEANUP_SHARED_ROOT=${hostPath(dependency.shared)}`, `-DNICKEL_CLEANUP_MANIFEST_SHA256=${dependency.manifestSha256}`,
        `-DNICKEL_CLEANUP_FIXTURE_ROOT=${hostPath(fixtures)}`];
      if (platform === 'win32') args.push(`-DCMAKE_PREFIX_PATH=${windowsToolsPrefix}`, '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded');
      tool('cmake', args);
      tool('cmake', ['--build', hostPath(build), '--parallel', '1']);
    }
    evidence.compilation = compilation(build, dependency, hostPath, hostRoot);
    const executable = path.join(build, platform === 'win32' ? 'palladium_cleanup_tests.exe' : 'palladium_cleanup_tests');
    if (!fs.existsSync(executable)) throw new Error('Exact same-generation cleanup executable is absent');
    evidence.executable = { path: executable, sha256: digest(fs.readFileSync(executable)), bytes: fs.statSync(executable).size };
    const bindingFile = path.join(work, 'built-binding.json');
    const binding = { format: 'palladium-cleanup-built-binding-v1', sourceSignature: signature,
      dependencyManifestSha256: dependency.manifestSha256, compilationSha256: evidence.compilation.commandsSha256, executable: evidence.executable };
    if (fs.existsSync(bindingFile)) {
      if (JSON.stringify(JSON.parse(fs.readFileSync(bindingFile))) !== JSON.stringify(binding)) throw new Error('Captured build binding changed; preserve it and create a new delta');
    } else if (configuration.mode === '--test') throw new Error('A test-only run requires an existing same-generation build binding');
    else fs.writeFileSync(bindingFile, `${JSON.stringify(binding, null, 2)}\n`, { flag: 'wx' });
    let runtimeFixtures = hostPath(fixtures);
    if (configuration.tmpfs) {
      fixtureLease = nativeFixtures.prepare(tool, `${signature.slice(0, 20)}-${process.pid}`);
      evidence.nativeFixtures = fixtureLease;
      runtimeFixtures = fixtureLease.path;
    }
    evidence.runtimeFixtures = runtimeFixtures;
    const runtime = configuration.distribution ? tool(hostPath(executable), [runtimeFixtures], true) : execute(executable, [fixtures], environment, evidence, true);
    evidence.runtimeStatus = runtime.status;
    const result = /^RESULT passed=(\d+) failed=(\d+) skipped=(\d+)$/m.exec(runtime.stdout);
    if (result) evidence.result = { passed: Number(result[1]), failed: Number(result[2]), skipped: Number(result[3]) };
    evidence.quarantineReproductions = [...runtime.stdout.matchAll(/^POSIX_QUARANTINE_REPRO iteration=(\d+) reached=(\d+) sameCtime=(\d+) restoredMtime=(\d+) conflict=(\d+) retainedNew=(\d+)$/gm)]
      .map(match => ({ iteration: Number(match[1]), reached: Number(match[2]), sameCtime: match[3] === '1', restoredMtime: match[4] === '1', conflictCount: Number(match[5]), retainedNew: match[6] === '1' }));
    if (runtime.status !== 0 || !result || evidence.result.failed !== 0) throw new Error('Cleanup runtime failed; complete raw result, executable and source bindings are retained');
  }
  evidence.ok = true;
} catch (error) {
  console.error(String(error));
  if (evidence) { evidence.ok = false; evidence.failure = String(error); }
  process.exitCode = 1;
} finally {
  if (evidence) {
    if (fixtureLease) {
      try { evidence.nativeFixtureAudit = nativeFixtures.auditAndRelease(fixtureTool, fixtureLease); }
      catch (error) { evidence.ok = false; evidence.fixtureAuditFailure = String(error); process.exitCode = 1; }
    }
    try {
      const after = snapshot.verifySnapshot(dependency.shared);
      if (after.manifestSha256 !== dependency.manifestSha256 || JSON.stringify(hashes()) !== JSON.stringify(evidence.sources)) throw new Error('Source or dependency changed during execution');
      evidence.sourceHashesMatch = true;
      if (evidence.executable && digest(fs.readFileSync(evidence.executable.path)) !== evidence.executable.sha256) throw new Error('Runtime executable changed during execution');
    } catch (error) {
      evidence.ok = false;
      evidence.bindingFailure = String(error);
      process.exitCode = 1;
    }
    try {
      capacity();
      evidence.finished = new Date().toISOString();
      const output = path.join(logDirectory, `run-${evidence.started.replace(/[:.]/g, '-')}-${process.pid}.json`);
      fs.writeFileSync(output, `${JSON.stringify(evidence, null, 2)}\n`, { flag: 'wx' });
      console.log(`EVIDENCE ${path.relative(root, output)}`);
    } catch (error) {
      console.error(`EVIDENCE_NOT_WRITTEN ${String(error)}`);
      process.exitCode = 1;
    }
  }
}
