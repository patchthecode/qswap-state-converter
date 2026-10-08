#!/usr/bin/env python3
"""Find an epoch-named QSwap state file and run the built production converter."""
import argparse
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent


def epoch_number(value):
    try:
        epoch = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError('Epoch must be a number from 1 to 999.')
    if not 1 <= epoch <= 999:
        raise argparse.ArgumentTypeError('Epoch must be a number from 1 to 999.')
    return epoch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node-dir', type=pathlib.Path, required=True,
                        help='Folder containing the epoch-named contract state files')
    parser.add_argument('--epoch', type=epoch_number, required=True,
                        help='Epoch of the saved state, for example 233')
    parser.add_argument('--owners', type=pathlib.Path, required=True,
                        help='Ownership list bound to this saved state')
    parser.add_argument('--output', type=pathlib.Path, required=True,
                        help='New converted state file')
    parser.add_argument('--report', type=pathlib.Path, required=True,
                        help='New verification report file')
    args = parser.parse_args()

    state = (args.node_dir.expanduser() / f'contract0013.{args.epoch:03d}').resolve()
    if not state.is_file():
        parser.error(f'QSwap state file not found: {state}\n'
                     'Check the folder and epoch. For a .000 snapshot or a renamed file, '
                     'use the direct converter command in README.md with its exact path.')

    binary = ROOT / 'build/converter/production/qswap-convert'
    if not binary.is_file():
        parser.error('Production converter is not built. From the repository folder, run:\n'
                     'python3 converter/build.py --profile production --core vendor/core')

    print(f'QSwap state: {state}', flush=True)
    command = [str(binary), str(state), str(args.owners.expanduser().resolve()),
               str(args.output.expanduser().resolve()), str(args.report.expanduser().resolve())]
    try:
        return subprocess.call(command)
    except OSError as error:
        parser.exit(1, f'Could not start the converter: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
