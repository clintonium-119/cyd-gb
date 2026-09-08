"""Test setup for the tools/ scripts.

The scripts import each other by module name because they all live in tools/ and
are run from the project root as `python tools/<name>.py`. The tests import them
the same way, so tools/ goes on sys.path here rather than in every suite.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import pytest


@pytest.fixture
def repo_root():
    """The project root, for tests that read committed headers or fixtures."""
    return REPO_ROOT
