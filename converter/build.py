#!/usr/bin/env python3
"""Build an explicit native ABI profile. No downloaded dependencies or source edits."""
import argparse
import hashlib
import json
import pathlib
import platform
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', choices=['production', 'local-fixture'], required=True)
parser.add_argument('--core', type=pathlib.Path, default=ROOT.parent / 'core')
args = parser.parse_args()
core = args.core.resolve()
lock = json.loads((ROOT / 'converter/source-lock.json').read_text())
for entry in lock['files']:
    base = core if entry['root'] == 'core' else ROOT
    actual = hashlib.sha256((base / entry['path']).read_bytes()).hexdigest()
    if actual != entry['sha256']:
        raise SystemExit(f"Source pin mismatch: {entry['root']}/{entry['path']}")

pools, users = (8192, 256) if args.profile == 'production' else (512, 64)
build = ROOT / 'build/converter' / args.profile
build.mkdir(parents=True, exist_ok=True)
schema = ['#pragma once', '#include <cstddef>']
for namespace, source in [('Legacy', 'reference/core/src/contracts/Qswap.h'), ('Patched', 'src/contracts/Qswap.h')]:
    text = (ROOT / source).read_text()
    schema += [f'namespace {namespace} {{', 'using namespace QPI;',
               f'constexpr uint64 QSWAP_MAX_POOL = {pools};',
               f'constexpr uint64 QSWAP_MAX_USER_PER_POOL = {users};']
    for name in ['PoolBasicState', 'LiquidityInfo', 'StateData']:
        found = re.findall(r'\tstruct ' + name + r'\n\t\{.*?\n\t\};', text, re.S)
        if len(found) != 1:
            raise SystemExit(f'Expected one exact declaration: {source}:{name}')
        declaration = found[0]
        # Start native object lifetimes before loading file bytes. These offline-only
        # constructors add no fields; all sizes and offsets are checked below.
        if name == 'PoolBasicState':
            declaration = declaration.replace('\n\t};', '\n\t\tPoolBasicState() : accFeePerLPX64(0) {}\n\t};')
        elif name == 'LiquidityInfo':
            declaration = declaration.replace('\n\t};', '\n\t\tLiquidityInfo() : liquidity(0), feeDebtX64(0), accumulatedFee(0) {}\n\t};')
        schema.append(declaration)
    schema.append('}')
fields = ['swapFeeRate', 'investRewardsFeeRate', 'shareholderFeeRate', 'poolCreationFeeRate',
          'investRewardsId', 'investRewardsEarnedFee', 'investRewardsDistributedAmount',
          'shareholderEarnedFee', 'shareholderDistributedAmount', 'mPoolBasicStates', 'mLiquidities',
          'qxFeeRate', 'burnFeeRate', 'qxEarnedFee', 'qxDistributedAmount', 'burnEarnedFee',
          'burnedAmount', 'cachedIssuanceFee', 'cachedTransferFee']
schema += [f'static_assert(offsetof(Legacy::StateData, {f}) == offsetof(Patched::StateData, {f}));' for f in fields]
old_size = 286326928 if args.profile == 'production' else 4501648
schema += [f'static_assert(sizeof(Legacy::StateData) == {old_size});',
           'static_assert(sizeof(Patched::StateData) == sizeof(Legacy::StateData));',
           'static_assert(sizeof(Legacy::LiquidityInfo) == 32);',
           'static_assert(alignof(Legacy::StateData) == 8 && alignof(Patched::StateData) == 8);',
           'static_assert(offsetof(Legacy::LiquidityInfo, feeDebtX64) == 8);',
           'static_assert(offsetof(Legacy::LiquidityInfo, accumulatedFee) == 24);',
           'static_assert(offsetof(Legacy::PoolBasicState, accFeePerLPX64) == 56);',
           'static_assert(sizeof(Legacy::PoolBasicState) == 72);',
           f'constexpr const char* BUILD_PROFILE = "{args.profile}";',
           f'constexpr const char* SOURCE_LOCK_SHA256 = "{hashlib.sha256((ROOT / "converter/source-lock.json").read_bytes()).hexdigest()}";']
tool_hash = hashlib.sha256()
for name in ['build.py', 'native.h', 'convert.cpp']:
    tool_hash.update(name.encode() + b'\0' + (ROOT / 'converter' / name).read_bytes())
schema.append(f'constexpr const char* TOOL_SOURCE_SHA256 = "{tool_hash.hexdigest()}";')
(build / 'schema.h').write_text('\n'.join(schema) + '\n')
command = ['clang++', '-std=c++17', '-O2', '-mavx2', '-mbmi', '-mlzcnt',
           '-Wno-undefined-inline', '-Wno-deprecated-declarations',
           '-I' + str(core / 'src'), '-I' + str(core), '-I' + str(build)]
if platform.system() == 'Darwin':
    command += ['-arch', 'x86_64']
elif platform.machine() not in ('x86_64', 'AMD64'):
    raise SystemExit('Linux build requires x86_64, matching Core native ABI')
command += [str(ROOT / 'converter/convert.cpp'), '-o', str(build / 'qswap-convert')]
if platform.system() != 'Darwin':
    command += ['-lcrypto']  # Explicit Linux dependency: OpenSSL development headers/library.
print(f'[build] Verified {len(lock["files"])} pinned inputs; compiling {args.profile}', flush=True)
subprocess.run(command, check=True)
binary = build / 'qswap-convert'
binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
info = {'profile': args.profile, 'command': command,
        'compiler': subprocess.check_output(['clang++', '--version'], text=True).strip(),
        'source_lock_sha256': hashlib.sha256((ROOT / 'converter/source-lock.json').read_bytes()).hexdigest(),
        'tool_source_sha256': tool_hash.hexdigest(),
        'schema_sha256': hashlib.sha256((build / 'schema.h').read_bytes()).hexdigest(),
        'binary_sha256': binary_sha}
(build / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
print(f'[build] Ready: {binary}', flush=True)
print(f'[build] Binary SHA256: {binary_sha}', flush=True)
