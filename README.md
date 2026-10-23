# QSwap pool and LP key converter (V2)

Offline conversion for the [QSwap contract in PR #1019](https://github.com/qubic/core/blob/15117e7947bdabfcea860ea6a8fd0b605380e4d4/src/contracts/Qswap.h). V2 replaces both truncated pool IDs and XOR LP keys, including IDs of empty created pools:

```text
pool ID = K12("QSWAPPL1" + full 32-byte issuer + 8-byte asset name)
LP key  = K12("QSWAPLP1" + new 32-byte pool ID + 32-byte account)
```

The original state fields, size and layout stay unchanged. Financial records and every byte outside pool IDs and the rebuilt LP collection are preserved. This tool reads local files and writes a separate state and JSON report. It does not download data, send transactions or install its output into a node.

**Use V2 bindings with this revision. The previous V1 manifest and LP-only converter are incompatible with this contract.**

## Build

Requires Python 3 and Clang/C++17. Linux: x86_64 with AVX2/BMI/LZCNT and OpenSSL development headers. macOS: Xcode command-line tools; Apple Silicon also needs Rosetta. Native Windows is unsupported; compatible x86_64 WSL 2 has not been tested. Use a local filesystem supporting POSIX hardlinks.

```bash
python3 converter/build.py --profile production --core vendor/core
```

The build verifies all 22 source pins and checks state sizes, alignment and field offsets. Dependencies are bundled; it downloads nothing. Vendored Core sources retain their license at `vendor/core/LICENSE.md` and existing copyright notices.

The production profile has 8,192 pool slots and 2,097,152 LP slots. Input and output are each 286,326,928 bytes. The explicit `local-fixture` build profile is only for reduced-capacity tests.

## Bindings for the exact input

Use a TSV file with real tabs, LF endings and no blank rows or comments:

```text
QSWAP_BINDINGS_V2<TAB><input state SHA-256>
P<TAB><legacy pool ID><TAB><full issuer>
L<TAB><legacy pool ID><TAB><account>
```

IDs are 64 lowercase hexadecimal characters encoding raw bytes, not ASCII Qubic addresses. The legacy pool ID contains the issuer's first 24 bytes followed by the asset name's eight bytes, in Core's byte order.

Provide exactly one `P` row for every created pool, including empty pools. Provide verified `L` candidates for every stored LP record, including locked minimums and stored-zero positions. Unused historical LP candidates are allowed; they create no records. Duplicate pairs, missing owners, ambiguous matches and inconsistent pool totals stop conversion.

Bindings must come from independently verified issuer and ownership evidence for that exact saved state. Matching an issuer prefix or XOR key checks consistency; it does not by itself prove the missing issuer bytes or ownership. This repository does not include a tool to prepare the final bindings.

## Run

For an epoch-named state file in a node's storage folder:

```bash
mkdir out
python3 converter/run.py \
  --node-dir /path/to/node-storage --epoch 235 \
  --owners /path/to/bindings-v2.tsv \
  --output out/converted-state.bin --report out/report.json
```

The existing `--owners` option now takes the V2 bindings file. The helper looks only for `contract0013.235` directly inside the supplied folder and prints its full path. Set the epoch to that of your saved file. It does not search subfolders, select the newest file or download anything.

For a `.000` snapshot or renamed file, use its exact path:

```bash
build/converter/production/qswap-convert \
  /path/to/saved-state /path/to/bindings-v2.tsv \
  out/converted-state.bin out/report.json
```

Arguments: `STATE BINDINGS.tsv NEW_STATE REPORT.json`. Output directories must exist and output paths must be new. Inputs are never overwritten. Accept only exit code 0 and a matching state/report pair with final `PASS`. Preserve the report, bindings and original input together. Publication of the two output files is not atomic across power loss.

## Historical example

Obtain `contract0013.233` from the [public epoch-233 archive](https://storage.qubic.li/network/233/ep233-full.zip). Use `examples/epoch233-bindings-v2.tsv` with that exact file:

```text
input SHA-256:  1ba465c10de3532e12a05731c3989558d626e54ac04ba6a1b041bb65e1a4f45a
output SHA-256: d762912172f9322160332462dce65727dc3b4d8d4b6acc5bfdd42e5eba23ab62
```

Expected: 148 LP records across 18 pools. This historical example is not the installation input for a future upgrade.

## Validation and deployment

The matching local source passed 39 focused converter cases and 20 QSwap contract tests in Core's adapted Linux harness. Saved production snapshots from epoch 233 (148 LP records) and epoch 235 (118 LP records, saved tick 85,398,975), each with 18 pools, produced byte-identical complete files on macOS and Linux. Pool IDs, LP keys, financial values, totals and minimum locks were independently checked. The packaged revision was also rebuilt on both platforms and checked against the saved epoch-235 output.

The activation input must be the final saved state after the old code finishes processing, with verified bindings for that exact file. Earlier snapshots omit later activity. Original and converted files have the same size and no format marker: an unconverted file can load, but its keys will not match the patched contract. Operators must install the matching converted state with the matching release.

A stock-node epoch-transition rehearsal and native Windows testing have not been completed. This is an external conversion tool; PR #1019 does not implement automatic or lazy migration.
