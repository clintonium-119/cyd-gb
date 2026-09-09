from datetime import datetime
from pathlib import Path
import shutil

Import("env")


def build_name(env):
    """Name the copy from the same two values compiled into the firmware.

    The pre-script leaves them in the shared SCons environment, so the file in
    builds/ and the string on the diagnostic page agree by inspection. Without
    them — a build that skipped the pre-script — the old local-time name is
    still a name.
    """
    version = env.get("CYD_BUILD_VERSION")
    stamp = env.get("CYD_BUILD_STAMP")
    if not version or not stamp:
        return f"gbscanner-{datetime.now().strftime('%Y%m%d_%H%M%S')}"

    # A describe of a branch-shaped tag carries slashes, which are not a file
    # name; the colon in the stamp is dropped for the same reason.
    version = version.replace("/", "-")
    stamp = stamp.replace(":", "")
    return f"cyd-gb-{version}-{stamp}"


def copy_firmware_with_timestamp(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    project_dir = Path(env.subst("$PROJECT_DIR"))
    firmware_bin = build_dir / f"{env.subst('$PROGNAME')}.bin"

    if not firmware_bin.exists():
        print(f"[timestamp] firmware not found: {firmware_bin}")
        return

    out_dir = project_dir / "builds"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / f"{build_name(env)}.bin"

    shutil.copy2(firmware_bin, out_file)
    print(f"[timestamp] copied to {out_file}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware_with_timestamp)
