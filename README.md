# ASCENT Picker

This repository publishes the standalone source subset and host checks.
An installable Widget Manager package and receiver qualification are pending.
Real captures, prebuilt binaries, credential files and private bootstrap are excluded.
Run python3 -I run_host_checks.py --output <fresh-directory>.


The host subset computes membership-based draft/configuration projections and
serializes the existing native line format. It contains no stateful PWM classifier,
transport, switch executor or second naming store. Configuration intent is not
observed runtime state. The caller must bind membership to the selected receiver.
Run `python3 -B -m unittest discover -s tests` with GCC available; the test bridge
compiles native parser/geometry/validation helpers and never runs a receiver daemon
or invokes an RF operation.
