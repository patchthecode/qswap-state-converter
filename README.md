# QSwap state converter

Offline conversion of QSwap's existing LP position keys to the K12 format used by the accompanying contract patch. It preserves liquidity, fees and pool reserves. It reads a saved state and ownership list, then writes a new state and JSON report.

## Build

Requires Python 3 and Clang/C++17. Linux: x86_64 with AVX2/BMI/LZCNT and OpenSSL development headers. macOS: Xcode command-line tools; Apple Silicon also needs Rosetta. Native Windows is unsupported; compatible x86_64 WSL 2 has not been tested. Use a local filesystem supporting POSIX hardlinks.

From this repository:

```bash
python3 converter/build.py --profile production --core vendor/core
```

The build verifies the included source fingerprints. Dependencies are bundled; it downloads nothing. The vendored Core sources retain their license at `vendor/core/LICENSE.md` and their existing copyright notices.

## Run

```bash
mkdir out
build/converter/production/qswap-convert \
  contract0013.233 OWNERS.tsv out/converted-state.bin out/report.json
```

Arguments: `STATE OWNERS.tsv NEW_STATE REPORT.json`. Use new output paths. Success means exit code 0 and `PASS`. Inputs are never modified or overwritten. Missing or ambiguous ownership, duplicate records, and inconsistent pool totals stop conversion.

`OWNERS.tsv` starts with `QSWAP_LP_OWNERS_V1<TAB><SHA-256 of STATE>`. Each following row contains `poolID<TAB>account`: 64 lowercase raw-byte hex characters per ID, with real tabs and LF endings. Every stored record, including locked and stored-zero positions, needs one unambiguous match. Unused candidates are allowed. A key match checks consistency; transaction history supplies the ownership evidence. A tool for preparing the final ownership list is not included.

## Historical conversion check

Obtain `contract0013.233` from the [public epoch-233 archive](https://storage.qubic.li/network/233/ep233-full.zip). Substitute `examples/epoch233-owners.tsv` for `OWNERS.tsv` above. Expected SHA-256:

```text
input:  1ba465c10de3532e12a05731c3989558d626e54ac04ba6a1b041bb65e1a4f45a
output: c54b820bd47d4dd4379ff4220968d9c1ff1b7d6e05af1578df71a64b4cdaf5c9
```

Expected: 148 records in 18 pools. This is rehearsal data, not the installation input for a future upgrade.

## State compatibility

The patched contract keeps the original state size and layout. Existing LP records still need their XOR keys converted to K12. File size alone does not identify the key format: an unconverted file can load, but its existing positions will not match the patched lookups.

The upgrade input is the final saved state after the old code finishes processing. A snapshot taken earlier omits any later deposits, withdrawals or swaps. Its ownership list covers records through the same closing tick and is bound to that file's SHA-256.

Identical inputs produce identical output fingerprints. A node epoch-transition rehearsal has not been run.
