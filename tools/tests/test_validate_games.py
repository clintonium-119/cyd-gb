"""Tests for tools/validate_games.py — what it prints and what it exits with."""

import json
import subprocess
import sys

import gamesdb
import validate_games

CONFORMING = [
    {
        "filename": "Tetris.gb",
        "title": "Tetris",
        "description": "Fit the falling blocks into complete rows.",
        "art": "",
        "shot": "",
        "manual": "",
        "starter": True,
        "developer": "Nintendo",
        "publisher": "Nintendo",
        "year": 1989,
        "genre": "Puzzle",
        "players": 2,
    }
]


def write_games(tmp_path, games):
    path = tmp_path / "games.json"
    path.write_text(json.dumps(games, indent=2) + "\n", encoding="utf-8")
    return path


def run_cli(repo_root, *args):
    """The CLI as CI runs it: a subprocess with neither directory in the env."""
    return subprocess.run(
        [sys.executable, "tools/validate_games.py", *args],
        cwd=repo_root,
        env={"PATH": "/usr/bin:/bin"},
        capture_output=True,
        text=True,
    )


def test_a_conforming_file_exits_zero_with_the_skipped_notices(repo_root, tmp_path):
    path = write_games(tmp_path, CONFORMING)
    result = run_cli(repo_root, "--games", str(path))

    assert result.returncode == 0, result.stdout + result.stderr
    assert "notice: ROM existence not checked" in result.stdout
    assert "notice: art, shot and manual existence not checked" in result.stdout
    assert "1 entries, 0 problems, 5 notices" in result.stdout


def test_the_committed_games_json_passes_in_repo_only_mode(repo_root):
    result = run_cli(repo_root)
    assert result.returncode == 0, result.stdout + result.stderr
    assert ", 0 problems," in result.stdout


def test_a_duplicate_filename_exits_one_and_names_the_entry(repo_root, tmp_path):
    path = write_games(tmp_path, CONFORMING + CONFORMING)
    result = run_cli(repo_root, "--games", str(path))

    assert result.returncode == 1
    assert "problem: Tetris.gb: filename duplicates entry 0" in result.stdout


def test_an_over_long_filename_exits_one_and_names_the_entry(repo_root, tmp_path):
    broken = json.loads(json.dumps(CONFORMING))
    broken[0]["filename"] = "a" * 61 + ".gb"
    path = write_games(tmp_path, broken)
    result = run_cli(repo_root, "--games", str(path))

    assert result.returncode == 1
    assert "filename is 64 bytes, the cap is 63" in result.stdout


def test_a_missing_file_is_a_problem_not_a_traceback(repo_root, tmp_path):
    result = run_cli(repo_root, "--games", str(tmp_path / "absent.json"))

    assert result.returncode == 1
    assert "problem: no such file" in result.stdout
    assert "Traceback" not in result.stderr


def test_malformed_json_is_a_problem_not_a_traceback(repo_root, tmp_path):
    path = tmp_path / "games.json"
    path.write_text("[{,}]", encoding="utf-8")
    result = run_cli(repo_root, "--games", str(path))

    assert result.returncode == 1
    assert "is not valid JSON" in result.stdout
    assert "Traceback" not in result.stderr


def test_strict_turns_a_skipped_check_into_a_problem(tmp_path, capsys):
    path = write_games(tmp_path, CONFORMING)
    status = validate_games.main(["--games", str(path), "--strict"])
    out = capsys.readouterr().out

    assert status == 1
    assert "--strict needs a ROM directory" in out
    assert "--strict needs a media directory" in out


def test_strict_passes_when_both_directories_are_given(tmp_path, capsys):
    rom_dir = tmp_path / "roms"
    rom_dir.mkdir()
    (rom_dir / "Tetris.gb").write_bytes(b"\x00")
    path = write_games(tmp_path, CONFORMING)

    status = validate_games.main(
        ["--games", str(path), "--strict", "--rom-dir", str(rom_dir),
         "--media-dir", str(tmp_path)]
    )
    out = capsys.readouterr().out

    assert status == 0
    assert "not checked" not in out


def test_a_round_trip_mismatch_is_reported_as_a_problem(tmp_path, capsys, monkeypatch):
    path = write_games(tmp_path, CONFORMING)
    monkeypatch.setattr(
        gamesdb, "round_trip", lambda games: ["Tetris.gb: title became 'Tetrsi'"]
    )
    status = validate_games.main(["--games", str(path)])
    out = capsys.readouterr().out

    assert status == 1
    assert "problem: Tetris.gb: title became 'Tetrsi'" in out


def test_the_round_trip_is_not_attempted_once_a_shape_is_broken(tmp_path, capsys, monkeypatch):
    broken = json.loads(json.dumps(CONFORMING))
    broken[0]["title"] = 17
    path = write_games(tmp_path, broken)

    def explode(games):
        raise AssertionError("round_trip must not run on a broken file")

    monkeypatch.setattr(gamesdb, "round_trip", explode)
    status = validate_games.main(["--games", str(path)])

    assert status == 1
    assert "title must be a string" in capsys.readouterr().out
