"""Tests for tools/factory_reset.py — no hardware. Nothing here erases anything."""

import pytest

import factory_reset


class Runner:
    """Stands in for subprocess.run so a test can see whether it was called."""

    def __init__(self, returncode=0):
        self.calls = []
        self.returncode = returncode

    def __call__(self, command):
        self.calls.append(command)
        return self


def test_the_command_erases_exactly_the_nvs_region(repo_root):
    table = factory_reset.parse_partitions(repo_root / "partitions.csv")
    offset, size = table["nvs"]

    command = factory_reset.reset_command(
        ["esptool"], "/dev/ttyUSB0", 921600, offset, size
    )

    assert command[-3:] == ["erase_region", "0x9000", "0x5000"]
    assert command[command.index("--chip") + 1] == "esp32"
    assert command[command.index("--port") + 1] == "/dev/ttyUSB0"
    assert command[command.index("--baud") + 1] == "921600"


def test_the_command_never_erases_the_whole_flash(repo_root):
    offset, size = factory_reset.parse_partitions(repo_root / "partitions.csv")["nvs"]
    command = factory_reset.reset_command(["esptool"], None, 921600, offset, size)

    assert "erase_flash" not in command
    assert "write_flash" not in command
    assert "--port" not in command


def test_the_erased_region_does_not_reach_app0_or_romdata(repo_root):
    """The region must end before app0 begins, or a reset would eat the app."""
    table = factory_reset.parse_partitions(repo_root / "partitions.csv")
    nvs_offset, nvs_size = table["nvs"]

    assert nvs_offset + nvs_size <= table["app0"][0]
    assert nvs_offset + nvs_size <= table["romdata"][0]


def test_dry_run_prints_the_command_and_calls_nothing(capsys):
    runner = Runner()
    status = factory_reset.main(["--dry-run"], runner=runner)
    captured = capsys.readouterr()

    assert status == 0
    assert runner.calls == []
    assert "erase_region 0x9000 0x5000" in captured.out


def test_dry_run_says_what_is_dropped_and_kept(capsys):
    factory_reset.main(["--dry-run"], runner=Runner())
    err = capsys.readouterr().err

    assert "pending cartridge write" in err
    assert "wizard flags" in err
    assert "save on the SD card" in err
    assert "no reflash needed" in err


def test_yes_runs_the_command_once(capsys):
    runner = Runner()
    status = factory_reset.main(["--yes"], runner=runner)

    assert status == 0
    assert len(runner.calls) == 1
    assert runner.calls[0][-3:] == ["erase_region", "0x9000", "0x5000"]
    assert "Power-cycle the unit" in capsys.readouterr().err


@pytest.mark.parametrize("answer", ["", "n", "N", "no", "Y ES", "yes please", "0x9000"])
def test_anything_but_y_declines_and_erases_nothing(answer, monkeypatch, capsys):
    monkeypatch.setattr("builtins.input", lambda prompt: answer)
    runner = Runner()

    status = factory_reset.main([], runner=runner)

    assert status == 1
    assert runner.calls == []
    assert "nothing was erased" in capsys.readouterr().err


@pytest.mark.parametrize("answer", ["y", "Y", " y ", "yes"[:1]])
def test_a_plain_y_accepts(answer, monkeypatch):
    monkeypatch.setattr("builtins.input", lambda prompt: answer)
    runner = Runner()

    assert factory_reset.main([], runner=runner) == 0
    assert len(runner.calls) == 1


def test_the_prompt_names_the_region(monkeypatch):
    seen = []
    monkeypatch.setattr("builtins.input", lambda prompt: seen.append(prompt) or "n")

    factory_reset.main([], runner=Runner())

    assert "0x9000" in seen[0]
    assert "0x5000" in seen[0]


def test_a_table_without_nvs_is_refused(tmp_path, capsys):
    csv = tmp_path / "partitions.csv"
    csv.write_text(
        "# no nvs here\n"
        "app0,     app,  factory, 0x10000,  0xA0000,\n",
        encoding="utf-8",
    )
    runner = Runner()

    status = factory_reset.main(["--partitions", str(csv), "--yes"], runner=runner)
    err = capsys.readouterr().err

    assert status == 1
    assert runner.calls == []
    assert str(csv) in err
    assert "no nvs partition" in err


def test_an_unreadable_table_is_refused(tmp_path, capsys):
    runner = Runner()
    status = factory_reset.main(
        ["--partitions", str(tmp_path / "absent.csv"), "--yes"], runner=runner
    )

    assert status == 1
    assert runner.calls == []
    assert "cannot read" in capsys.readouterr().err


def test_a_failing_esptool_propagates_its_status(capsys):
    runner = Runner(returncode=2)
    assert factory_reset.main(["--yes"], runner=runner) == 2
    assert "Power-cycle" not in capsys.readouterr().err


def test_the_offset_is_not_hard_coded_in_the_script(repo_root):
    """The region must come from partitions.csv, never from a literal."""
    source = (repo_root / "tools" / "factory_reset.py").read_text(encoding="utf-8")

    assert "0x9000" not in source
    assert "0x5000" not in source
    assert "parse_partitions" in source
