# ASCENT Picker

This repository publishes the standalone source subset and host checks.
Standalone Widget Manager releases are pending. See the receiver status below.
Real captures, prebuilt binaries, credential files and private bootstrap are excluded.
Run python3 -I run_host_checks.py --output <fresh-directory>.


The host subset computes membership-based draft/configuration projections and
serializes the existing native line format. It contains no stateful PWM classifier,
transport, switch executor or second naming store. Configuration intent is not
observed runtime state. The caller must bind membership to the selected receiver.
Run `python3 -B -m unittest discover -s tests` with GCC available; the test bridge
compiles native parser/geometry/validation helpers and never runs a receiver daemon
or invokes an RF operation.

## Receiver status

| Receiver | Implementation and package status |
| --- | --- |
| Classic VRX | The repository includes Classic-specific pair release and explicit peer selection. The integrated AC+ 0.43.61 build showed stable direct HDMI switching across three tested positions and receiver-side RC switching on a Classic bench. The Classic owner build is below. The standalone Classic Widget Manager package and lifecycle validation are pending. |
| VRX MAX | The standalone build and archive recipe is below. On-device qualification of the standalone archive and its Widget Manager lifecycle is pending. |

### MAX receiver build

With AArch64 GCC and binutils installed, build into a separate directory and
create the archive and its digest inventory:

```sh
sh receiver/build-max.sh /tmp/picker-max-build
python3 receiver/package-max.py /tmp/picker-max-build /tmp/picker-max.zip
```

This is the MAX runtime and startup package. Receiver installation, update and
rollback require the approved Widget Manager release inventory and qualification.

### Classic VRX owner build

This recipe targets the Classic Gnd485 receiver on Ground 19.2.14. It enables
the Classic pair-release and explicit-peer switching path and links against the
exact libc for that profile. Supply that libc from your own authorized copy.

```sh
python3 receiver/build-classic.py --libc /path/to/libc-2.25.so --output /tmp/picker-classic-owner
```

The build produces `ascent_picker_rt` and a digest receipt. Use the Classic
recipe only for the specified profile. The MAX archive is for VRX MAX.

Package a build matching the reviewed Classic owner with:

```sh
python3 receiver/package-classic.py /tmp/picker-classic-owner /tmp/picker-classic.zip
```

The owner archive depends on Classic native payload revision 16 for the
launcher, display and RC support. This archive is a native integration
candidate. Widget Manager admission and Classic install, update and rollback
validation are pending.

### Combined host and receiver qualification bundle

Use the reviewed Classic installer ZIP and MAX receiver variant ZIP as inputs:

```sh
python3 receiver/package-bundle.py /path/to/classic.zip /path/to/max.zip /tmp/picker-bench.zip
```

The recipe verifies both variant digests and packages the repository's host
projection code. The output is a qualification candidate and requires a
reviewed AC+ package pin and lifecycle checks before receiver deployment.
