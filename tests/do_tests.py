#!/usr/bin/env python3

#
# shr test harness
#
# This script should be run in the source directory shr/tests.
# It expects, as a parameter, the path to the build directory.
#
#

from sys import argv
from os.path import isfile
from glob import glob
from subprocess import run

def main(argv):
  bindir = argv[1] if (len(argv) > 1) else "../build/tests"

  # get list of compiled tests in binary dir
  all_tests = glob("test*[0-9]", root_dir=bindir)

  # use only the tests having .ans files
  ans_tests = [x for x in all_tests if isfile(x + ".ans")]

  # sort tests by numeric suffix
  tests = sorted(ans_tests, key=lambda name: int(name[4:]))

  # form path to each test in bindir so PATH isn't searched
  qualified_tests = [bindir + "/" + x for x in tests]

  # form a trio of lists that we can iterate through tuples
  paths = zip(tests, qualified_tests, [x+".ans" for x in tests])

  for test, path, ans in paths:
    print(test, flush=True)
    out = run([path], capture_output=True, text=True)
    if (out.stderr):
      print(out.stderr)
    with open(ans, 'r') as ansfile:
      anstext = ansfile.read()
    if (out.stdout != anstext):
      print(test + " failed")

if __name__ == "__main__":
  main(argv)
