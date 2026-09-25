"""Tests for tools/flash.py — no hardware, no network."""

import hashlib
import re
import sys

import pytest

import flash


def fake_platformio(root):
    """A PlatformIO core dir holding esptool and boot_app0."""
    esptool = root / flash.ESPTOOL_RELPATH
    esptool.parent.mkdir(parents=True)
    esptool.write_text("# esptool\n", encoding="utf-8")

    boot_app0 = root / flash.BOOT_APP0_RELPATH
    boot_app0.parent.mkdir(parents=True)
    boot_app0.write_bytes(b"\x00" * 8192)
    return esptool, boot_app0


def build_dir_with_images(root):
    root.mkdir(parents=True, exist_ok=True)
    for name in (flash.BOOTLOADER_NAME, flash.PARTITIONS_NAME, flash.FIRMWARE_NAME):
        (root / name).write_bytes(name.encode())
    return root


# --- locating esptool -----------------------------------------------------


def test_find_esptool_prefers_platformio_core_dir(tmp_path, monkeypatch):
    esptool, _ = fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))

    assert flash.find_esptool() == [sys.executable, str(esptool)]


def test_find_esptool_falls_back_to_the_home_platformio(tmp_path, monkeypatch):
    esptool, _ = fake_platformio(tmp_path / "home" / ".platformio")
    monkeypatch.delenv("PLATFORMIO_CORE_DIR", raising=False)
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))

    assert flash.find_esptool() == [sys.executable, str(esptool)]


def test_find_esptool_falls_back_to_path(tmp_path, monkeypatch):
    monkeypatch.delenv("PLATFORMIO_CORE_DIR", raising=False)
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    monkeypatch.setattr(
        flash.shutil, "which", lambda name: "/usr/bin/esptool" if name == "esptool" else None
    )

    assert flash.find_esptool() == ["/usr/bin/esptool"]


def test_find_esptool_names_every_place_it_looked(tmp_path, monkeypatch):
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    monkeypatch.setattr(flash.shutil, "which", lambda name: None)

    with pytest.raises(RuntimeError) as error:
        flash.find_esptool()

    message = str(error.value)
    assert str(tmp_path / "core") in message
    assert str(tmp_path / "home" / ".platformio") in message
    assert "PATH" in message


def test_find_esptool_resolves_on_this_machine():
    """The acceptance criterion: PlatformIO's package is what gets used here."""
    resolved = flash.find_esptool()
    assert resolved[0] == sys.executable
    assert resolved[1].endswith("tool-esptoolpy/esptool.py")


# --- partitions.csv -------------------------------------------------------


def test_parse_partitions_reads_the_committed_table(repo_root):
    table = flash.parse_partitions(repo_root / "partitions.csv")

    assert table["nvs"] == (0x9000, 0x5000)
    assert table["app0"] == (0x10000, 0xA0000)
    assert table["romdata"] == (0xB0000, 0x350000)


def test_parse_partitions_ignores_comments_and_blank_lines(repo_root):
    table = flash.parse_partitions(repo_root / "partitions.csv")

    # The committed file is 3 rows under a 15-line comment header.
    assert sorted(table) == ["app0", "nvs", "romdata"]


def test_parse_partitions_accepts_decimal_and_suffixed_sizes(tmp_path):
    csv = tmp_path / "partitions.csv"
    csv.write_text(
        "# a comment\n"
        "\n"
        "nvs,      data, nvs,     36864,    20K,\n"
        "app0,     app,  factory, 0x10000,  1M,\n",
        encoding="utf-8",
    )
    table = flash.parse_partitions(csv)

    assert table["nvs"] == (36864, 20 * 1024)
    assert table["app0"] == (0x10000, 1024 * 1024)


def test_the_firmware_offset_equals_app0_in_the_table(repo_root):
    """The one thing that must not drift between the table and IMAGES."""
    table = flash.parse_partitions(repo_root / "partitions.csv")
    firmware_offset = next(
        offset for offset, name in flash.IMAGES if name == flash.FIRMWARE_NAME
    )

    assert firmware_offset == table["app0"][0]


# --- the command ----------------------------------------------------------


def test_flash_command_mirrors_platformios_flags(tmp_path, monkeypatch):
    _, boot_app0 = fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    build = build_dir_with_images(tmp_path / "build")

    command = flash.flash_command(
        ["esptool"], "/dev/ttyUSB0", 921600, flash.resolve_images(build)
    )

    assert command[:1] == ["esptool"]
    assert "--chip" in command and command[command.index("--chip") + 1] == "esp32"
    assert command[command.index("--port") + 1] == "/dev/ttyUSB0"
    assert command[command.index("--baud") + 1] == "921600"
    assert command[command.index("--before") + 1] == "default_reset"
    assert command[command.index("--after") + 1] == "hard_reset"
    assert "write_flash" in command and "-z" in command
    # dio, not the qio platformio.ini asks for: the platform builder rewrites it.
    assert command[command.index("--flash_mode") + 1] == "dio"
    assert command[command.index("--flash_freq") + 1] == "80m"
    assert command[command.index("--flash_size") + 1] == "4MB"


def test_flash_command_lists_every_image_in_platformios_order(tmp_path, monkeypatch):
    _, boot_app0 = fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    build = build_dir_with_images(tmp_path / "build")

    command = flash.flash_command(
        ["esptool"], None, 921600, flash.resolve_images(build)
    )
    tail = command[command.index("4MB") + 1:]

    assert tail == [
        "0x1000", str(build / flash.BOOTLOADER_NAME),
        "0x8000", str(build / flash.PARTITIONS_NAME),
        "0xe000", str(boot_app0),
        "0x10000", str(build / flash.FIRMWARE_NAME),
    ]


def test_no_port_flag_is_emitted_when_no_port_is_given(tmp_path, monkeypatch):
    fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    build = build_dir_with_images(tmp_path / "build")

    command = flash.flash_command(["esptool"], None, 921600, flash.resolve_images(build))

    assert "--port" not in command
    assert "/dev/ttyUSB0" not in " ".join(command)


def test_no_reset_leaves_the_board_in_its_bootloader(tmp_path, monkeypatch, capsys):
    fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    build = build_dir_with_images(tmp_path / "build")

    assert flash.main(["--build-dir", str(build), "--no-reset", "--dry-run"]) == 0
    command = capsys.readouterr().out.split()
    assert command[command.index("--after") + 1] == "no_reset"
    # Only that: every other flag is the default's.
    assert command.count("--after") == 1


def test_a_build_dir_without_the_firmware_is_refused(tmp_path, monkeypatch):
    fake_platformio(tmp_path / "core")
    monkeypatch.setenv("PLATFORMIO_CORE_DIR", str(tmp_path / "core"))
    monkeypatch.setattr(flash.Path, "home", staticmethod(lambda: tmp_path / "home"))
    (tmp_path / "empty").mkdir()

    with pytest.raises(RuntimeError, match="pio run -e cyd"):
        flash.resolve_images(tmp_path / "empty")


# --- SHA256SUMS -----------------------------------------------------------


def write_release(directory, tampered=None, omit=None):
    directory.mkdir(parents=True, exist_ok=True)
    names = [
        flash.BOOTLOADER_NAME, flash.PARTITIONS_NAME,
        flash.BOOT_APP0_NAME, flash.FIRMWARE_NAME,
    ]
    lines = []
    for name in names:
        data = name.encode() * 8
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {name}")
        if name == omit:
            continue
        if name == tampered:
            data = data + b"!"
        (directory / name).write_bytes(data)
    (directory / flash.SUMS_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return directory


def test_verify_sums_passes_an_intact_release(tmp_path):
    assert flash.verify_sums(write_release(tmp_path / "rel")) == []


def test_verify_sums_names_a_tampered_asset(tmp_path):
    directory = write_release(tmp_path / "rel", tampered=flash.FIRMWARE_NAME)
    assert flash.verify_sums(directory) == [flash.FIRMWARE_NAME]


def test_verify_sums_names_a_missing_asset(tmp_path):
    directory = write_release(tmp_path / "rel", omit=flash.BOOT_APP0_NAME)
    assert flash.verify_sums(directory) == [flash.BOOT_APP0_NAME]


def test_verify_sums_reports_an_absent_sums_file(tmp_path):
    (tmp_path / "rel").mkdir()
    assert flash.verify_sums(tmp_path / "rel") == [flash.SUMS_NAME]


def test_a_release_missing_an_image_is_refused(tmp_path):
    directory = write_release(tmp_path / "rel", omit=flash.BOOT_APP0_NAME)
    with pytest.raises(RuntimeError, match=flash.BOOT_APP0_NAME):
        flash.resolve_release_images(directory)


# --- the origin repo ------------------------------------------------------


@pytest.mark.parametrize(
    ("url", "expected"),
    [
        ("git@github.com:clintonium-119/cyd-gb.git", "clintonium-119/cyd-gb"),
        ("https://github.com/clintonium-119/cyd-gb.git", "clintonium-119/cyd-gb"),
        ("https://github.com/clintonium-119/cyd-gb", "clintonium-119/cyd-gb"),
    ],
)
def test_origin_repo_parses_both_url_shapes(url, expected, monkeypatch):
    class Result:
        returncode = 0
        stdout = url + "\n"

    monkeypatch.setattr(flash.subprocess, "run", lambda *a, **k: Result())
    assert flash.origin_repo() == expected


# --- the CLI --------------------------------------------------------------


def test_dry_run_prints_the_command_and_writes_nothing(capsys):
    status = flash.main(["--dry-run"])
    out = capsys.readouterr().out

    assert status == 0
    assert "write_flash" in out
    assert "--flash_mode dio" in out
    assert out.count("0x10000") == 1


def test_build_dir_and_release_are_mutually_exclusive():
    with pytest.raises(SystemExit):
        flash.main(["--build-dir", ".", "--release", "v0.1.0"])


def stub_gh(tmp_path, monkeypatch, tampered=None):
    """A `gh` that populates the download directory instead of hitting GitHub.

    Exercises the whole --release path — download, verify, assemble — without a
    network or a published release.
    """
    calls = []
    real_run = flash.subprocess.run

    def fake_run(command, *args, **kwargs):
        if command[:1] == ["gh"]:
            calls.append(command)
            directory = tmp_path / command[command.index("-D") + 1]
            write_release(directory, tampered=tampered)

            class Result:
                returncode = 0

            return Result()
        return real_run(command, *args, **kwargs)

    monkeypatch.setattr(flash.subprocess, "run", fake_run)
    return calls


def test_release_dry_run_downloads_verifies_and_prints_the_command(
    tmp_path, monkeypatch, capsys
):
    monkeypatch.chdir(tmp_path)
    calls = stub_gh(tmp_path, monkeypatch)

    status = flash.main(["--release", "v0.1.0", "--repo", "owner/name", "--dry-run"])
    captured = capsys.readouterr()

    assert status == 0
    assert calls and calls[0][:4] == ["gh", "release", "download", "v0.1.0"]
    assert "--repo" in calls[0] and "owner/name" in calls[0]
    assert flash.SUMS_NAME in " ".join(calls[0])
    assert f"{flash.SUMS_NAME} verified for v0.1.0" in captured.err
    assert "write_flash" in captured.out
    # All four images come from the release, boot_app0 included.
    for _, name in flash.IMAGES:
        assert name in captured.out


def test_a_tampered_release_aborts_before_any_flash(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    stub_gh(tmp_path, monkeypatch, tampered=flash.FIRMWARE_NAME)

    status = flash.main(["--release", "v0.1.0", "--repo", "owner/name"])
    captured = capsys.readouterr()

    assert status == 1
    assert f"does not match {flash.SUMS_NAME}: {flash.FIRMWARE_NAME}" in captured.err
    assert "refusing to flash an unverified release" in captured.err
    assert "write_flash" not in captured.out


def test_verify_sums_reads_the_format_sha256sum_actually_writes(tmp_path):
    """`sha256sum ./*.bin`, which is what the release workflow runs."""
    directory = tmp_path / "rel"
    directory.mkdir()
    lines = []
    for _, name in flash.IMAGES:
        data = name.encode() * 4
        (directory / name).write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()}  ./{name}")
    (directory / flash.SUMS_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")

    assert flash.verify_sums(directory) == []

    (directory / flash.FIRMWARE_NAME).write_bytes(b"tampered")
    assert flash.verify_sums(directory) == [flash.FIRMWARE_NAME]


def test_the_release_workflow_publishes_every_image_flash_py_expects(repo_root):
    """The asset names are the contract between the workflow and this tool."""
    workflow = (repo_root / ".github" / "workflows" / "release.yml").read_text(
        encoding="utf-8"
    )

    for _, name in flash.IMAGES:
        assert name in workflow, f"release.yml never mentions {name}"
    assert flash.SUMS_NAME in workflow
    # boot_app0.bin is not in the build directory, so it must be copied from
    # the framework package rather than from .pio.
    assert "framework-arduinoespressif32" in workflow


def test_the_release_workflow_only_triggers_on_a_v_tag(repo_root):
    workflow = (repo_root / ".github" / "workflows" / "release.yml").read_text(
        encoding="utf-8"
    )

    assert "tags:" in workflow
    assert "'v*'" in workflow
    assert "contents: write" in workflow
    # Not a branch trigger: a release comes from a tag and nothing else.
    assert "branches:" not in workflow


def test_the_two_workflows_pin_the_same_platformio(repo_root):
    ci = (repo_root / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    release = (repo_root / ".github" / "workflows" / "release.yml").read_text(
        encoding="utf-8"
    )
    pin = re.search(r"platformio==([\d.]+)", ci)

    assert pin is not None
    assert f"platformio=={pin.group(1)}" in release
