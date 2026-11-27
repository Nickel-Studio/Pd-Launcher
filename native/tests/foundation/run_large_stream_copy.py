import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import time

CHUNK_BYTES = 64 * 1024
LARGE_BYTES = 4 * 1024 * 1024 * 1024 + 137
LARGE_RESERVE_BYTES = 4 * 1024 * 1024 * 1024
OVERHEAD_BYTES = 64 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def native(path):
    value = str(path)
    if os.name != 'nt':
        return value
    require(not value.startswith(('\\\\?\\', '\\\\.\\')), 'Scratch paths must remain logical DOS/UNC paths')
    return '\\\\?\\UNC\\' + value[2:] if value.startswith('\\\\') else '\\\\?\\' + value


def invoke(probe, arguments, timeout=120):
    result = subprocess.run([str(probe), *map(str, arguments)], capture_output=True, text=True,
                            encoding='utf-8', errors='strict', timeout=timeout)
    require(len(result.stdout.encode('utf-8')) <= 64 * 1024 and len(result.stderr.encode('utf-8')) <= 64 * 1024,
            'Probe exceeded its bounded diagnostic output')
    if result.stdout:
        print(result.stdout, end='', flush=True)
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr, flush=True)
    require(result.returncode in (0, 77), 'Native probe failed with exit ' + str(result.returncode))
    require(result.stdout.strip(), 'Native probe returned no observable result')
    return result.returncode, json.loads(result.stdout)


def digest_file(path):
    digest = hashlib.sha256()
    total = 0
    with open(native(path), 'rb', buffering=0) as stream:
        while True:
            data = stream.read(CHUNK_BYTES)
            if not data:
                break
            digest.update(data)
            total += len(data)
    return total, digest.hexdigest()


def generate(source, size, scratch, reserve):
    digest = hashlib.sha256()
    written = 0
    started = time.monotonic()
    with open(native(source), 'xb', buffering=0) as stream:
        while written < size:
            remaining = size - written
            required = remaining + size + reserve + OVERHEAD_BYTES
            require(shutil.disk_usage(native(scratch)).free >= required,
                    'Output volume reserve fell below source+destination budget during generation')
            data = os.urandom(min(CHUNK_BYTES, remaining))
            view = memoryview(data)
            while view:
                count = stream.write(view)
                require(count is not None and 0 < count <= len(view), 'Generated file write made invalid progress')
                view = view[count:]
            digest.update(data)
            written += len(data)
        os.fsync(stream.fileno())
        require(os.fstat(stream.fileno()).st_size == size, 'Generated source has the wrong real EOF')
    return {'bytesWritten': written, 'sha256': digest.hexdigest(), 'generatorChunkBytes': CHUNK_BYTES,
            'generationSeconds': time.monotonic() - started, 'generator': 'os.urandom sequential writes + fsync',
            'sparseCreationUsed': False, 'wholeFileBufferUsed': False}


def execute(arguments):
    scratch = Path(arguments.scratch_root)
    probe = Path(arguments.probe)
    require(scratch.is_absolute() and probe.is_absolute(), 'Explicit absolute scratch root and probe required')
    require(scratch.is_dir() and probe.is_file(), 'Existing scratch directory and built canonical probe required')
    evidence_path = Path(arguments.evidence) if arguments.evidence else None
    if evidence_path is not None:
        require(evidence_path.is_absolute() and not evidence_path.exists() and evidence_path.parent.is_dir(),
                'Evidence path must be absolute, unused and have an existing parent')
    root = scratch / ('pd-foundation-copy-' + secrets.token_hex(16))
    require(not root.exists(), 'Unique experiment root already exists')
    if arguments.mode == 'budget':
        formula_code, formula = invoke(probe, ['budget', scratch, LARGE_BYTES, LARGE_RESERVE_BYTES])
        require(formula['requiredBytes'] == 12952011026 and formula['sourceBytes'] == formula['destinationBytes'] == LARGE_BYTES and
                formula['reserveBytes'] == LARGE_RESERVE_BYTES and formula['overheadBytes'] == OVERHEAD_BYTES and
                formula['fixtureWrites'] == 0 and formula['allowed'] == (formula_code == 0) and not root.exists(),
                'Exact physical large-copy budget or refusal status is incorrect')
        free = shutil.disk_usage(native(scratch)).free
        refused_reserve = free + OVERHEAD_BYTES
        code, budget = invoke(probe, ['budget', scratch, LARGE_BYTES, refused_reserve])
        require(code == 77 and not budget['allowed'] and budget['fixtureWrites'] == 0 and not root.exists(),
                'Insufficient capacity was not rejected before fixture creation')
        print('PASS Palladium/Pd streaming-copy budget: exact 12952011026-byte gate and actual capacity refusal before fixture writes')
        return 0
    large = arguments.mode == 'large'
    size = LARGE_BYTES if large else 256 * 1024 + 137
    reserve = LARGE_RESERVE_BYTES if large else 128 * 1024 * 1024
    code, budget = invoke(probe, ['budget', scratch, size, reserve])
    require(budget['requiredBytes'] == size * 2 + reserve + OVERHEAD_BYTES and budget['fixtureWrites'] == 0,
            'Native budget does not reserve both full files and headroom before writes')
    if code == 77:
        require(not budget['allowed'] and not root.exists(), 'Capacity refusal created a fixture or reported permission')
        print('SKIP Palladium/Pd physical streaming copy: ' + json.dumps(budget, sort_keys=True))
        return 77
    require(budget['allowed'], 'Native budget was not granted')
    allocation_code, allocation = invoke(probe, ['allocation-check'])
    require(allocation_code == 0 and allocation['allocationCeilingVerified'] and allocation['liveAllocationCeilingVerified'],
            'C++ single/live allocation meter self-test failed')
    deep = root / 'destination'
    while len(str(deep).encode('utf-16-le')) // 2 < 549:
        deep /= 'Pd copy \u6df1 space-012345678901234567890123456789'
    source = root / 'source' / 'fixture.bin'
    destination = deep / 'pack.mrpack'
    prepared = False
    try:
        prepare_code, setup = invoke(probe, ['prepare', root, deep])
        require(prepare_code == 0 and setup['prepared'], 'Guarded fixture creation failed')
        prepared = True
        generated = generate(source, size, scratch, reserve)
        copy_code, observed = invoke(probe, ['copy', source, destination, size, generated['sha256'], reserve],
                                     timeout=3600 if large else 120)
        require(copy_code == 0 and observed['copied'] and observed['bytes'] == size, 'Production copy did not commit the full file')
        require(observed['cppAllocationCeilingBytes'] == 1024 * 1024 and
                observed['largestCppAllocationBytes'] <= observed['cppAllocationCeilingBytes'] and
                observed['liveCppAllocationCeilingBytes'] == 4 * 1024 * 1024 and
                observed['peakLiveCppAllocationBytes'] <= observed['liveCppAllocationCeilingBytes'] and
                observed['liveCppAllocationBytesAfterCopy'] == 0 and observed['cppAllocationRequests'] > 0,
                'Copy used unbounded/live C++ allocations or bypassed allocation measurement')
        require(observed['residentCeilingBytes'] == 128 * 1024 * 1024 and
                observed['peakResidentBytes'] <= observed['residentCeilingBytes'], 'Native process resident peak is unbounded')
        require(observed['sourceBefore'] == observed['sourceAfter'], 'Source identity, length or revision changed')
        require(observed['sourceAfter']['allocatedBytes'] >= size and observed['destinationFile']['allocatedBytes'] >= size,
                'Source/destination are not fully allocated ordinary files')
        require(observed['freeAfterCopy'] >= reserve, 'Output volume reserve was exhausted during the physical copy')
        source_bytes, source_hash = digest_file(source)
        destination_bytes, destination_hash = digest_file(destination)
        require(source_bytes == destination_bytes == size and source_hash == destination_hash == generated['sha256'],
                'Independent complete-file hashes or EOF lengths differ')
        with os.scandir(native(deep)) as entries:
            require(not any(entry.name.startswith('.nickel-copy-') for entry in entries), 'Production copy left an owned stage')
        evidence = {'schemaVersion': 1, 'product': 'Palladium/Pd', 'purpose': 'foundation streaming-copy experiment',
                    'mode': arguments.mode, 'genuineAtLeast4GiBCopy': large and size > 4 * 1024 * 1024 * 1024,
                    'actualSourceBytes': source_bytes, 'actualDestinationBytes': destination_bytes,
                    'sourceSha256After': source_hash, 'destinationSha256': destination_hash,
                    'budget': budget, 'generation': generated, 'nativeCopy': observed,
                    'allocationMeter': 'global C++ new/new[] including aligned/nothrow forms, fixed live registry; native-library allocations are not intercepted; OS process resident peak is separately bounded'}
        if evidence_path is not None:
            with open(native(evidence_path), 'x', encoding='utf-8', newline='\n') as stream:
                json.dump(evidence, stream, ensure_ascii=True, indent=2)
                stream.write('\n')
        print(json.dumps(evidence, sort_keys=True, ensure_ascii=True), flush=True)
        print('PASS Palladium/Pd ' + ('physical >4 GiB' if large else 'small physical') + ' streaming copy: ' + str(size) +
              ' bytes, source preserved, independent SHA256, max C++ allocation ' + str(observed['largestCppAllocationBytes']))
        return 0
    finally:
        if prepared:
            # Only this run's unique synthetic tree is removed; scratch itself is never cleaned.
            shutil.rmtree(native(root))


parser = argparse.ArgumentParser()
parser.add_argument('--probe', required=True)
parser.add_argument('--scratch-root', required=True)
parser.add_argument('--mode', choices=['small', 'budget', 'large'], required=True)
parser.add_argument('--evidence')
options = parser.parse_args()
try:
    sys.exit(execute(options))
except Exception as error:
    print('FAIL Palladium/Pd streaming-copy experiment: ' + str(error), file=sys.stderr)
    sys.exit(1)
