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
from distutils import sysconfig


SOURCE_FILE = 'expy_local_scan.c'


def patch_makefile(source_dir, build_dir):
    makefile_name = os.path.join(build_dir, 'Local', 'Makefile')

    makefile = open(makefile_name, 'r').readlines()

    cfg = sysconfig.get_config_var

    cflags = '-I%s %s' % (cfg('INCLUDEPY'), cfg('CFLAGSFORSHARED'))
    extralibs = '%s -lpython%s' % (cfg('LDFLAGS'), cfg('VERSION'))
    source = os.path.join('Local', SOURCE_FILE)
    has_options = True
    have_local_scan = True

    #
    # Look for existing CFLAGS and EXTRALIBS lines, and append info.
    # Note if this has been done by setting cflags and extralibs to None
    #
    # Look for LOCAL_SCAN_SOURCE and LOCAL_SCAN_HAS_OPTIONS lines, replace
    # if necessary.
    #
    for i in range(len(makefile)):
        if makefile[i].startswith('CFLAGS=') and cflags:
            makefile[i] = makefile[i].rstrip() + ' ' + cflags.strip() + '\n'
            cflags = None
        if makefile[i].startswith('EXTRALIBS=') and extralibs:
            makefile[i] = makefile[i].rstrip() + ' ' + extralibs + '\n'
            extralibs = None
        if makefile[i].startswith('LOCAL_SCAN_SOURCE='):
            makefile[i] = 'LOCAL_SCAN_SOURCE=' + source + '\n'
            source = None
        if makefile[i].startswith('LOCAL_SCAN_HAS_OPTIONS='):
            makefile[i] = 'LOCAL_SCAN_HAS_OPTIONS=yes\n'
            has_options = False
        if makefile[i].startswith('HAVE_LOCAL_SCAN='):
            makefile[i] = 'HAVE_LOCAL_SCAN=yes\n'
            have_local_scan = False

    # Didn't update/replace existing lines? append new ones
    #
    if cflags:
        makefile.append('CFLAGS=%s\n' % cflags)
    if extralibs:
        makefile.append('EXTRALIBS=%s\n' % extralibs)
    if source:
        makefile.append('LOCAL_SCAN_SOURCE=%s\n' % source)
    if has_options:
        makefile.append('LOCAL_SCAN_HAS_OPTIONS=yes\n')
    if have_local_scan:
        makefile.append('HAVE_LOCAL_SCAN=yes\n')

    #
    # Write out updated makefile
    #
    makefile = ''.join(makefile)
    open(makefile_name, 'w').write(makefile)

    #
    # Symlink in C sourcefile
    #
    os.symlink(os.path.join(source_dir, SOURCE_FILE), os.path.join(build_dir, 'Local', SOURCE_FILE))


if __name__ == '__main__':
    import sys
    if len(sys.argv) < 2:
        print 'Attempt to patch Exim makefile to support Python local_scan'
        print '    Usage: %s <build_dir>' % sys.argv[0]
        print ''
        print 'Suggested path for your local_scan module:'
        print '    ', os.path.join(sysconfig.get_python_lib(), 'exim_local_scan.py')
        sys.exit(1)

    build_dir = sys.argv[1]
    source_dir = os.path.abspath(os.path.dirname(sys.argv[0]))

    patch_makefile(source_dir, build_dir)

