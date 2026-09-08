"""Flash a board from the local env:cyd build or from a tagged GitHub Release.

The flashing station's first half. It uses the esptool PlatformIO already
ships — the copy whose version matches the toolchain, never a pip install — and
issues the same command `pio run -e cyd -t upload` issues, mirrored from the
installed platform's builder rather than guessed at. See the decision note on
the flag set: PlatformIO rewrites qio to dio on the way to esptool, writes
boot_app0.bin even though this table has no otadata, and puts the app offset
last.

It also carries the two helpers the factory reset shares — locating esptool and
parsing partitions.csv — so there is one copy of each.

    pio run -e cyd && python tools/flash.py --dry-run
    python tools/flash.py --port /dev/ttyUSB0
    python tools/flash.py --release v0.1.0

A release is verified against its SHA256SUMS before anything is written; a
tampered or missing asset aborts. Standard library only, plus esptool from
PlatformIO and `gh` for --release.

Run from the project root.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# The board is an esp32; platformio.ini pins espressif32@6.5.0.
CHIP = "esp32"

# platformio.ini's upload_speed for env:cyd. The board JSON says 460800; the
# project overrides it.
DEFAULT_BAUD = 921600

DEFAULT_BUILD_DIR = REPO_ROOT / ".pio" / "build" / "cyd"
PARTITIONS_CSV = REPO_ROOT / "partitions.csv"

# Every one of these mirrors what PlatformIO passes; none is derived from
# platformio.ini directly. In particular the mode is dio and NOT the qio the
# project asks for: the platform builder rewrites qio to dio for write_flash,
# and the real mode reaches the chip through the bootloader image's own header.
FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "4MB"
BEFORE_RESET = "default_reset"
AFTER_RESET = "hard_reset"

# The asset names, which are the contract with the release workflow.
BOOTLOADER_NAME = "bootloader.bin"
PARTITIONS_NAME = "partitions.bin"
BOOT_APP0_NAME = "boot_app0.bin"
FIRMWARE_NAME = "firmware.bin"
SUMS_NAME = "SHA256SUMS"

# Offset and file, in the order PlatformIO writes them — the app offset last,
# because UPLOADCMD appends it after the extra images.
IMAGES = (
    (0x1000, BOOTLOADER_NAME),   # esp32 bootloader; 0x0000 on later chips
    (0x8000, PARTITIONS_NAME),   # the partition table, generated from our CSV
    (0xE000, BOOT_APP0_NAME),    # written unconditionally by the framework
    (0x10000, FIRMWARE_NAME),    # ESP32_APP_OFFSET, and app0 in partitions.csv
)

# Where boot_app0.bin lives inside PlatformIO's Arduino framework package.
BOOT_APP0_RELPATH = Path("packages") / "framework-arduinoespressif32" / "tools" / \
    "partitions" / BOOT_APP0_NAME

ESPTOOL_RELPATH = Path("packages") / "tool-esptoolpy" / "esptool.py"


def platformio_dirs():
    """The PlatformIO core directories to search, in precedence order."""
    dirs = []
    override = os.environ.get("PLATFORMIO_CORE_DIR", "")
    if override != "":
        dirs.append(Path(override))
    dirs.append(Path.home() / ".platformio")
    return dirs


def find_esptool():
    """The esptool to run, as an argv prefix.

    PlatformIO's own copy first, because its version is the one the pinned
    toolchain was tested with. A pip-installed esptool is deliberately not
    considered; only one already on PATH, as a last resort.
    """
    for base in platformio_dirs():
        candidate = base / ESPTOOL_RELPATH
        if candidate.is_file():
            return [sys.executable, str(candidate)]

    for name in ("esptool", "esptool.py"):
        found = shutil.which(name)
        if found is not None:
            return [found]

    searched = [str(base / ESPTOOL_RELPATH) for base in platformio_dirs()]
    raise RuntimeError(
        "no esptool found. Looked for "
        + ", then ".join(searched)
        + ", then esptool or esptool.py on PATH. Run `pio run -e cyd` once to "
          "install PlatformIO's copy."
    )


def find_boot_app0():
    """boot_app0.bin from the framework package; it is not in the build dir."""
    for base in platformio_dirs():
        candidate = base / BOOT_APP0_RELPATH
        if candidate.is_file():
            return candidate
    searched = [str(base / BOOT_APP0_RELPATH) for base in platformio_dirs()]
    raise RuntimeError(
        f"no {BOOT_APP0_NAME} found. Looked for " + ", then ".join(searched)
    )


def parse_size(token):
    """A partitions.csv offset or size: hex, decimal, or a K/M suffix."""
    token = token.strip()
    if token == "":
        return None
    multiplier = 1
    if token[-1] in "kK":
        multiplier = 1024
        token = token[:-1]
    elif token[-1] in "mM":
        multiplier = 1024 * 1024
        token = token[:-1]
    return int(token, 0) * multiplier


def parse_partitions(csv_path):
    """name -> (offset, size) from a partitions.csv.

    Comments and blank lines are skipped. Shared with the factory reset, which
    needs the nvs region and must never learn its offset from a literal.
    """
    table = {}
    with open(csv_path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if line == "":
                continue
            fields = [field.strip() for field in line.split(",")]
            if len(fields) < 5:
                continue
            name = fields[0]
            offset = parse_size(fields[3])
            size = parse_size(fields[4])
            if name == "" or offset is None or size is None:
                continue
            table[name] = (offset, size)
    return table


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_sums(directory):
    """Check every file SHA256SUMS names. Returns the names that did not match.

    A named file that is absent is a mismatch, not a skip: a release missing an
    image is exactly the case this has to refuse.
    """
    directory = Path(directory)
    sums = directory / SUMS_NAME
    if not sums.is_file():
        return [SUMS_NAME]

    mismatched = []
    for line in sums.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line == "":
            continue
        parts = line.split()
        if len(parts) < 2:
            mismatched.append(line)
            continue
        # `sha256sum ./*.bin` writes "./firmware.bin"; a binary-mode line
        # from some tools writes "*firmware.bin". The asset is uploaded and
        # downloaded under its bare name either way.
        expected, name = parts[0], parts[-1].lstrip("*")
        if name.startswith("./"):
            name = name[2:]
        path = directory / name
        if not path.is_file() or sha256_of(path) != expected:
            mismatched.append(name)
    return mismatched


def resolve_images(build_dir):
    """The four images from a local build; boot_app0 from the framework."""
    build_dir = Path(build_dir)
    resolved = []
    for offset, name in IMAGES:
        if name == BOOT_APP0_NAME:
            resolved.append((offset, find_boot_app0()))
            continue
        path = build_dir / name
        if not path.is_file():
            raise RuntimeError(
                f"no {name} in {build_dir}; run `pio run -e cyd` first"
            )
        resolved.append((offset, path))
    return resolved


def resolve_release_images(directory):
    """The four images from a downloaded release, all as published assets."""
    directory = Path(directory)
    resolved = []
    for offset, name in IMAGES:
        path = directory / name
        if not path.is_file():
            raise RuntimeError(f"the release has no {name}")
        resolved.append((offset, path))
    return resolved


def flash_command(esptool, port, baud, images):
    """The esptool argv, mirroring PlatformIO's UPLOADERFLAGS and UPLOADCMD."""
    command = [*esptool, "--chip", CHIP]
    if port:
        command += ["--port", str(port)]
    command += [
        "--baud", str(baud),
        "--before", BEFORE_RESET,
        "--after", AFTER_RESET,
        "write_flash", "-z",
        "--flash_mode", FLASH_MODE,
        "--flash_freq", FLASH_FREQ,
        "--flash_size", FLASH_SIZE,
    ]
    for offset, path in images:
        command += [f"0x{offset:x}", str(path)]
    return command


def origin_repo():
    """owner/name from the git origin, for `gh release download --repo`."""
    result = subprocess.run(
        ["git", "remote", "get-url", "origin"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    match = re.search(r"[:/]([^/:]+/[^/]+?)(?:\.git)?\s*$", result.stdout.strip())
    return match.group(1) if match else None


def download_release(tag, repo, directory):
    """Fetch the release's binaries and SHA256SUMS into directory."""
    command = [
        "gh", "release", "download", tag,
        "-p", "*.bin", "-p", SUMS_NAME,
        "-D", str(directory),
    ]
    if repo:
        command += ["--repo", repo]
    subprocess.run(command, check=True)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port",
        help="serial port; omitted means esptool's own auto-detect",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--build-dir",
        default=str(DEFAULT_BUILD_DIR),
        help="a local PlatformIO build directory",
    )
    source.add_argument("--release", metavar="TAG", help="flash a tagged release")
    parser.add_argument("--repo", help="owner/name; defaults to the git origin")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the esptool command and exit without touching a board",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    try:
        esptool = find_esptool()
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="cyd-release-") as download_dir:
        try:
            if args.release:
                repo = args.repo or origin_repo()
                download_release(args.release, repo, download_dir)
                mismatched = verify_sums(download_dir)
                if mismatched:
                    for name in mismatched:
                        print(f"does not match {SUMS_NAME}: {name}", file=sys.stderr)
                    print("refusing to flash an unverified release", file=sys.stderr)
                    return 1
                print(f"{SUMS_NAME} verified for {args.release}", file=sys.stderr)
                images = resolve_release_images(download_dir)
            else:
                images = resolve_images(args.build_dir)
        except (RuntimeError, subprocess.CalledProcessError) as error:
            print(str(error), file=sys.stderr)
            return 1

        # A self-check rather than a second source of truth: the app image has
        # to land on app0, or the table and this list have drifted apart.
        table = parse_partitions(PARTITIONS_CSV)
        app_offset = table.get("app0", (None, None))[0]
        firmware_offset = next(
            offset for offset, name in IMAGES if name == FIRMWARE_NAME
        )
        if app_offset != firmware_offset:
            print(
                f"partitions.csv puts app0 at 0x{app_offset:x} but this tool "
                f"writes the firmware at 0x{firmware_offset:x}",
                file=sys.stderr,
            )
            return 1

        command = flash_command(esptool, args.port, args.baud, images)

        if args.dry_run:
            print(" ".join(command))
            return 0

        print(" ".join(command), file=sys.stderr)
        return subprocess.run(command).returncode


if __name__ == "__main__":
    sys.exit(main())
