#!/usr/bin/env python3
"""MATLAB for t-code: a MATLAB session (matlab -nodesktop) in a terminal tab, .m files run in it,
run errors as diagnostics. The work is mlang.py's, shared with the octave plugin."""
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from mlang import Matlab, run  # noqa: E402

if __name__ == "__main__":
    run(Matlab())
