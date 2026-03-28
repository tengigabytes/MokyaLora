#!/usr/bin/env python3
"""
fetch_data.py — Download MIE data sources to a local directory.

Downloads the data files required by gen_dict.py and gen_font.py:
  - tsi.csv          (libchewing-data, pinned commit)
  - en_50k.txt       (hermitdave/FrequencyWords — 50 k English words with frequencies)
  - google-10000-english-no-swears.txt  (first20hours — fallback, no frequencies)
  - unifont-17.0.04.otf

Usage:
  python fetch_data.py [--data-dir PATH] [--skip-if-exists]

Requires: Python ≥ 3.8, standard library only.
"""

import argparse
import sys
import urllib.request
from pathlib import Path

# ── Pinned data sources ────────────────────────────────────────────────────
# URLs are pinned to specific commits / releases for reproducibility.
# Update the hermitdave commit hash when a newer snapshot is desired.

SOURCES = {
    'tsi.csv': (
        'https://raw.githubusercontent.com/chewing/libchewing-data/'
        'ea74f76dd2548b1d65d0ff70c3ae66057a6ad97d/dict/chewing/tsi.csv'
    ),
    # ── English 50k (primary) ────────────────────────────────────────────
    # hermitdave/FrequencyWords  content/2018/en/en_50k.txt
    # Format: "word frequency" (single space), sorted by frequency descending.
    # Frequencies are raw corpus occurrence counts — very large numbers.
    # See also: https://github.com/hermitdave/FrequencyWords
    # NOTE: pin to a specific commit hash for fully reproducible builds,
    # e.g. replace "master" with a SHA-1 once the build is validated.
    'en_50k.txt': (
        'https://raw.githubusercontent.com/hermitdave/FrequencyWords/'
        'master/content/2018/en/en_50k.txt'
    ),
    # ── English 10k fallback (no frequencies) ───────────────────────────
    # Useful when the 50k source is unavailable or when a smaller EN dict
    # is preferred.  Without frequencies all words receive freq=1, so
    # candidate ordering is not meaningful.
    'google-10000-english-no-swears.txt': (
        'https://raw.githubusercontent.com/first20hours/google-10000-english/'
        'master/google-10000-english-no-swears.txt'
    ),
    'unifont-17.0.04.otf': (
        'https://unifoundry.com/pub/unifont/unifont-17.0.04/'
        'font-builds/unifont-17.0.04.otf'
    ),
}

DEFAULT_DATA_DIR = Path(__file__).parent.parent / 'data_sources'

# ── Download helpers ───────────────────────────────────────────────────────

def _validate_not_html(path: Path) -> None:
    """Raise RuntimeError if the file looks like an HTML error page."""
    with open(path, 'rb') as f:
        head = f.read(8)
    if head[:5] in (b'<!DOC', b'<!doc', b'<html', b'<HTML'):
        path.unlink(missing_ok=True)
        raise RuntimeError(
            f"Downloaded file appears to be an HTML error page, not the expected data.\n"
            f"URL may be wrong or the server returned an error.\n"
            f"Deleted: {path}"
        )


def download(name: str, url: str, dest: Path, skip_if_exists: bool) -> None:
    if skip_if_exists and dest.exists() and dest.stat().st_size > 0:
        print(f"  [skip]  {name}  (already exists, {dest.stat().st_size:,} bytes)")
        return

    print(f"  Downloading {name} ...")
    print(f"    from: {url}")
    print(f"    to:   {dest}")

    try:
        urllib.request.urlretrieve(url, dest)
    except Exception as exc:
        dest.unlink(missing_ok=True)
        raise RuntimeError(f"Download failed for {name}: {exc}") from exc

    size = dest.stat().st_size
    _validate_not_html(dest)
    print(f"    OK  ({size:,} bytes)")

# ── Argument parsing ───────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Download MIE data sources (dict + font) to a local directory.')
    p.add_argument('--data-dir', default=str(DEFAULT_DATA_DIR),
                   help=f'Destination directory  [default: {DEFAULT_DATA_DIR}]')
    p.add_argument('--skip-if-exists', action='store_true',
                   help='Skip files that already exist with non-zero size')
    return p.parse_args()

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    args     = parse_args()
    data_dir = Path(args.data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)

    print(f"Data directory: {data_dir}")
    print()

    errors = []
    for name, url in SOURCES.items():
        dest = data_dir / name
        try:
            download(name, url, dest, args.skip_if_exists)
        except RuntimeError as exc:
            print(f"  ERROR: {exc}", file=sys.stderr)
            errors.append(name)

    print()
    if errors:
        print(f"FAILED: {len(errors)} file(s) could not be downloaded: {', '.join(errors)}",
              file=sys.stderr)
        sys.exit(1)
    else:
        print(f"All {len(SOURCES)} files ready in {data_dir}")
        print()
        print("Next steps — recommended build (ZH + EN 50k, <4 MB total):")
        print(f"  python gen_dict.py \\")
        print(f"      --libchewing {data_dir}/tsi.csv \\")
        print(f"      --zh-max-abbr-syls 4 \\")
        print(f"      --en-wordlist {data_dir}/en_50k.txt \\")
        print(f"      --en-max-per-key 5 \\")
        print(f"      --emit-charlist <build-dir>/data/charlist.txt \\")
        print(f"      --output-dir <build-dir>/data")
        print()
        print("Alternative — 10k fallback (no frequencies, useful for testing):")
        print(f"  python gen_dict.py \\")
        print(f"      --en-wordlist {data_dir}/google-10000-english-no-swears.txt \\")
        print(f"      --en-max-per-key 5 \\")
        print(f"      --output-dir <build-dir>/data")
        print()
        print("Font generation:")
        print(f"  python gen_font.py  --unifont {data_dir}/unifont-17.0.04.otf \\")
        print(f"                      --charlist <build-dir>/data/charlist.txt \\")
        print(f"                      --out <build-dir>/data/mie_unifont_sm_16.bin")


if __name__ == '__main__':
    main()
