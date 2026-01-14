#!/usr/bin/env python2
"""
Attempt to figure out what options need to be added to the Exim
Makefile to embed a Python interpreter for your particular platform.

Most of the info seems to be available in the Python distutils.sysconfig
module, cross your fingers.

  2002-10-19 Barry Pederson <bp@barryp.org>

"""
import os
import os.path
import shutil

EXTRALIBS = " ".join(
    (
        "-lz",
        "-lm",
        "-lpthread",
        "-ldl",
        "-lutil",
        "-Xlinker",
        "-export-dynamic",
        "-Wl,-O1",
        "-Wl,-Bsymbolic-functions",
    )
)

CONFIG = {
    "python2": {
        "CFLAGS": "-fPIC",
        "INCLUDE": "-I/usr/local/include/python2.7",
        "EXTRALIBS": "/usr/local/lib/python2.7/config/libpython2.7.a "
        + EXTRALIBS,
        "LOCAL_SCAN_SOURCE": "expy_local_scan.c",
    },
    "python3.9": {
        "CFLAGS": "-fPIC",
        "INCLUDE": "-I/usr/include/python3.9",
        "EXTRALIBS": "-lpython3.9 " + EXTRALIBS,
        "LOCAL_SCAN_SOURCE": "expy_local_scan_py3.c",
    },
    "python3.11": {
        "CFLAGS": "-fPIC",
        "INCLUDE": "-I/usr/include/python3.11",
        "EXTRALIBS": "-lpython3.11 " + EXTRALIBS,
        "LOCAL_SCAN_SOURCE": "expy_local_scan_py3.c",
    },
}


def patch_makefile(source_dir, build_dir, python_version):
    makefile_name = os.path.join(build_dir, "Local", "Makefile")

    with open(makefile_name, "r") as fd:
        makefile = fd.readlines()

    makefile.append("CFLAGS+=%s\n" % CONFIG[python_version]["CFLAGS"])
    makefile.append("INCLUDE+=%s\n" % CONFIG[python_version]["INCLUDE"])
    makefile.append("EXTRALIBS+=%s\n" % CONFIG[python_version]["EXTRALIBS"])
    makefile.append(
        "LOCAL_SCAN_SOURCE=%s\n"
        % os.path.join("Local", CONFIG[python_version]["LOCAL_SCAN_SOURCE"])
    )
    makefile.append("LOCAL_SCAN_HAS_OPTIONS=yes\n")
    makefile.append("HAVE_LOCAL_SCAN=yes\n")

    # Write out updated makefile
    makefile = "".join(makefile)

    with open(makefile_name, "w") as fd:
        fd.write(makefile)

    # Symlink in C sourcefile
    shutil.copy(
        os.path.join(source_dir, CONFIG[python_version]["LOCAL_SCAN_SOURCE"]),
        os.path.join(
            build_dir, "Local", CONFIG[python_version]["LOCAL_SCAN_SOURCE"]
        ),
    )


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("Attempt to patch Exim makefile to support Python local_scan")
        print("    Usage: %s <build_dir> <python2/python3>" % sys.argv[0])
        print("")
        sys.exit(1)

    build_dir = sys.argv[1]
    source_dir = os.path.abspath(os.path.dirname(sys.argv[0]))
    python_version = sys.argv[2]
    if python_version not in ("python2", "python3.9", "python3.11"):
        raise ValueError(
            "Invalid python_version: %r. "
            "Must be 'python2', 'python3.9' or 'python3.11'." % python_version
        )
    patch_makefile(source_dir, build_dir, python_version)
