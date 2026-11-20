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
| Classic VRX | The repository includes Classic-specific pair release and explicit peer selection. The integrated AC+ 0.43.61 build showed stable direct HDMI switching across three tested positions and receiver-side RC switching on a Classic bench. The standalone Classic build recipe, package and Widget Manager lifecycle validation are pending. |
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
