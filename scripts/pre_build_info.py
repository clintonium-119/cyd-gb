from datetime import datetime, timezone
from pathlib import Path
import subprocess

Import("env")


def git_describe(project_dir):
    """The tree's own name for itself, or "unknown".

    A release tarball has no .git, and a machine building from one has no
    business failing over it — the version line on the diagnostic page then
    says "unknown", which is the truth.
    """
    try:
        done = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=project_dir,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    if done.returncode != 0:
        return "unknown"
    return done.stdout.strip() or "unknown"


def c_string(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def write_build_info(env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    gen_dir = Path(env.subst("$BUILD_DIR")) / "gen"
    gen_dir.mkdir(parents=True, exist_ok=True)

    version = git_describe(project_dir)
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")

    body = (
        '#include "build_info.h"\n'
        "\n"
        f'const char BUILD_FW_VERSION[] = "{c_string(version)}";\n'
        f'const char BUILD_TIME_UTC[] = "{c_string(stamp)}";\n'
    )

    # Only rewrite when the content changed, so a rebuild inside the same
    # minute on an unchanged tree does not recompile this translation unit.
    # It still changes once a minute, which costs one object file.
    out = gen_dir / "build_info.cpp"
    if not out.exists() or out.read_text() != body:
        out.write_text(body)

    env["CYD_BUILD_VERSION"] = version
    env["CYD_BUILD_STAMP"] = stamp
    print(f"[build_info] {version} {stamp}")

    return gen_dir


# The native environment compiles nothing from src/ and has no board to stamp.
if env["PIOENV"] == "cyd":
    gen = write_build_info(env)
    # Generated into the build directory, never into src/ or include/: a build
    # leaves the working tree clean. Objects land beside it for the same
    # reason.
    # BuildSources() compiles with the global environment, which does not
    # carry the project's include/ the way the src/ build does — so the
    # generated unit could not find its own header without this.
    env.Append(CPPPATH=[env.subst("$PROJECT_INCLUDE_DIR")])
    env.BuildSources(str(Path(env.subst("$BUILD_DIR")) / "gen_obj"), str(gen))
