# OBS RTMP Server

**An RTMP server inside OBS Studio.** Add an *RTMP Server* source, point a camera, a phone or another computer at
it, and the live video and audio show up in your scene. No nginx, no media server, nothing to install besides the
plugin.

[![Protocol tests](https://github.com/davvore33/obs-rtmp-server/actions/workflows/protocol-tests.yml/badge.svg)](https://github.com/davvore33/obs-rtmp-server/actions/workflows/protocol-tests.yml)
[![Build (macOS)](https://github.com/davvore33/obs-rtmp-server/actions/workflows/build-macos.yml/badge.svg)](https://github.com/davvore33/obs-rtmp-server/actions/workflows/build-macos.yml)
![License: GPL-2.0-or-later](https://img.shields.io/badge/plugin-GPL--2.0--or--later-blue)
![License: LGPL-2.1-or-later](https://img.shields.io/badge/rtmp%20library-LGPL--2.1--or--later-blue)
![OBS 32](https://img.shields.io/badge/OBS%20Studio-32.x-302E31)

```
  DJI Osmo Pocket 3 ─┐
  phone streaming app ┼── rtmp://192.168.1.20/live/<key> ──▶  OBS Studio
  ffmpeg, another OBS ┘                                       └─ RTMP Server source ──▶ your scene
```

## Why

OBS can *send* RTMP to Twitch or YouTube, but it has no way to *receive* it. The usual workaround for getting a
wireless action camera, a phone or a second PC into a scene is to run nginx-rtmp (or a similar media server) next to
OBS and add a Media Source that plays from it. This plugin removes that middle layer: the server lives in OBS, and
the stream arrives as a normal source with video, audio and its own entry in the mixer.

## Highlights

- **A normal OBS source.** Pick the port, application name and stream key in the source's properties; the
  properties dialog shows the exact URL to type into your encoder, and a status line tells you what is happening.
- **Several cameras, one port.** Give each source its own stream key (`…/live/cam1`, `…/live/cam2`).
- **Recovers by itself.** If the camera drops off Wi-Fi and comes back, the source resumes without you touching
  OBS: a sender that has gone silent is replaced by the same key reconnecting after 5 seconds.
- **Modern codecs.** H.264 and HEVC, plus AV1 and VP9 through Enhanced RTMP; AAC, MP3 and Opus audio.
- **Nothing extra to install.** Decoding uses the FFmpeg that is already inside OBS.
- **Safe defaults.** It only listens on the local machine until you choose otherwise, and a stream key is always
  required (see [Security](#security)).
- **Tested beyond "it compiles".** Protocol unit tests, a fuzzer, sanitizer runs, and an end-to-end test that loads
  the built plugin into a real libobs and streams a known pattern into it (see [Development](#development)).

## Quick start

1. [Install the plugin](#installation) and restart OBS.
2. In your scene click **+ → RTMP Server**. Leave the defaults for now: OBS listens on this computer only, at
   `rtmp://127.0.0.1/live` with the stream key `stream`.
3. Send something to it. With FFmpeg installed, a test pattern and a tone:

   ```bash
   ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
          -c:v libx264 -preset veryfast -c:a aac -f flv rtmp://127.0.0.1/live/stream
   ```

   (If you build from source you also get `test-publisher`, which needs no FFmpeg:
   `build_macos/Release/test-publisher rtmp://127.0.0.1/live/stream --seconds 30`.)

To receive from another device, set **Listen on** to *All network interfaces*, change the stream key to something
hard to guess, and use this computer's address instead of `127.0.0.1`.

## Compatibility

| Sender | Status |
| --- | --- |
| FFmpeg | Tested automatically (H.264 with and without B-frames, HEVC, AAC, video-only, audio-only) |
| DJI Osmo Pocket 3 (RTMP live stream over Wi-Fi) | Confirmed working |
| OBS Studio as a sender, phone streaming apps, other cameras and encoders | Expected to work, not yet tested. Reports welcome |

| Platform | Status |
| --- | --- |
| macOS, Apple silicon, OBS 32 | Built (for macOS 13+) and tested on macOS 26 |
| macOS (Intel) | Universal binary is built; the Intel slice has not been run |
| Windows, Linux | Not built or tested. The RTMP library is compiled and tested on Linux in CI; the plugin glue and build scripts are the official OBS plugin template, so porting should be small. Help welcome |

## Installation

### macOS, from a release

1. Download `obs-rtmp-server-<version>-macos-universal.zip` from the
   [Releases](https://github.com/davvore33/obs-rtmp-server/releases) page and unzip it.
2. Move `obs-rtmp-server.plugin` into
   `~/Library/Application Support/obs-studio/plugins/` (in Finder: **Go → Go to Folder…**, paste the path).
3. The build is signed ad hoc, not notarized, so macOS marks a downloaded copy as quarantined. Clear the mark once:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/"Application Support"/obs-studio/plugins/obs-rtmp-server.plugin
   ```
4. Restart OBS. In the log (**Help → Log Files → View Current Log**) you should see
   `[obs-rtmp-server] plugin loaded`.

To remove it, delete the `.plugin` folder.

### From source (macOS)

Needs Xcode and CMake 3.28 or newer.

```bash
git clone https://github.com/davvore33/obs-rtmp-server.git
cd obs-rtmp-server
cmake --preset macos                         # first run downloads OBS sources and dependencies to .deps/ (~1 GB)
cmake --build build_macos --config Release
cp -R build_macos/Release/obs-rtmp-server.plugin ~/Library/"Application Support"/obs-studio/plugins/
```

## Connecting a sender

Whatever the sender is, it needs two things: the **server** (`rtmp://<address>/<application>`) and the **stream
key**. The properties of the source show both, with the address filled in for you.

> **Many cameras and apps have a single "RTMP URL" field.** Then the key goes at the end of the URL, after a slash:
> `rtmp://192.168.1.20/live/stream`. Pasting only the server part (`rtmp://192.168.1.20/live`) makes the sender
> connect without a key, and the plugin refuses it.

### DJI Osmo Pocket 3

Choose the RTMP live stream option in the DJI Mimo app and enter the full URL, key included, e.g.
`rtmp://192.168.1.20/live/stream`. The camera (or the phone it streams through) must be on the same network as the
computer, and the source's **Listen on** must be *All network interfaces*.

### OBS Studio (as the sender)

*Settings → Stream → Service: Custom…*, **Server** `rtmp://192.168.1.20/live`, **Stream Key** `stream`. (Here the
two are separate fields, so the key does not go in the server.)

### FFmpeg

```bash
ffmpeg -re -i input.mp4 -c copy -f flv rtmp://192.168.1.20/live/stream
```

### Phone apps and others

Any app that can publish to a custom RTMP server works the same way: server `rtmp://<address>/live`, key as
configured, or the one-line form if it only has a single URL field.

## Source settings

| Setting | Default | Notes |
| --- | --- | --- |
| Listen on | This computer only (`127.0.0.1`) | *All network interfaces* (`0.0.0.0`) lets other devices connect. macOS asks once whether OBS may accept incoming connections |
| Port | `1935` | The standard RTMP port. Sources can share a port if their keys differ |
| Application | `live` | The first path segment of the URL |
| Stream key | `stream` | Change it before exposing the port to a network. Avoid `?` in keys |
| Server URL | (read-only) | Built from the settings above, with this computer's address filled in |
| Latency | Normal | *Normal* lets OBS buffer so audio and video stay in sync. *Low* shows frames as they arrive: less delay, but audio and video can drift |

The status line at the top reads *Waiting for a stream…*, *Receiving a stream from …* (with resolution and codecs),
or an error such as *Cannot start the server: … Address already in use*. Press **Refresh status** to update it.

## Troubleshooting

When something does not connect, the OBS log is the fastest guide. Look for lines starting with
`[obs-rtmp-server]` (**Help → Log Files → View Current Log**).

| You see | Meaning | Fix |
| --- | --- | --- |
| `listening: rtmp://…` | The server is up | Nothing to fix |
| Status: *Cannot start the server: … Address already in use* | Another program (or OBS source) is using the port with different settings | Pick another port, or quit the other program |
| No connection attempts in the log at all | The sender cannot reach this computer | Check *Listen on* is *All network interfaces*, both devices are on the same network, and the macOS firewall allows OBS |
| `publish rejected for live/ (No stream is listening…)` with an **empty key** after the slash | The sender connected without a stream key | Put the key at the end of the URL: `rtmp://<address>/live/<key>` |
| `publish rejected for live/xyz (No stream is listening…)` | The key or application does not match any source | Compare them with the source's settings (they are case sensitive) |
| `publish rejected … (This stream already has a publisher.)` | Something else is actively streaming to that key | Stop the other sender, or use a different key. (A sender that has been silent for 5 seconds, such as a camera that lost Wi-Fi, is replaced automatically by the next one to connect.) |
| `ignoring unsupported video packets` / `audio packets` | The sender uses a codec this plugin does not read (for example Sorenson H.263, Nellymoser) | Switch the sender to H.264 + AAC |
| Picture but no sound (or the reverse) | The sender sent only one track, or its codec is unsupported | Check the `receiving video/audio:` lines in the log |
| Audio and video out of sync | *Latency: Low* is on | Switch back to *Normal* |

When you report a problem, please include the sender model or app, the `[obs-rtmp-server]` lines from the log, and
your OBS version.

## Security

- **The default listens on `127.0.0.1` only**, so nothing outside your computer can reach it.
- With *All network interfaces*, anyone who can reach the port **and knows the stream key** can send video into
  your scene. The stream key is the only credential and it travels unencrypted (there is no RTMPS). Use a long
  random key, and only on networks you trust, never on the open internet.
- The server parses data from the network, so every size, count and nesting depth is bounded, connections that
  stall or never start publishing are dropped, and at most 8 connections per port are accepted. The parsers are
  fuzzed and run clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.

## Supported formats and limits

| | |
| --- | --- |
| Transport | RTMP over TCP with the plain handshake; AMF0 commands (also inside AMF3 messages) |
| Video | H.264; HEVC (Enhanced RTMP and the older codec-id-12 form); AV1 and VP9 (Enhanced RTMP) |
| Audio | AAC, MP3, Opus (Enhanced RTMP) |

Not supported: playback from the server (it only *receives*), encrypted RTMPE, RTMPS/TLS, and the Flash-era
"digest" handshake (no current encoder needs it). Decoding is in software. AV1, VP9, MP3 and Opus are implemented
but have not been tested end to end.

## Development

### Layout

```
src/rtmp/                    RTMP ingest server library (LGPL-2.1-or-later): no OBS or FFmpeg dependency
  hub.*       shares ports between routes; one publisher per (application, key)
  server.*    TCP listener, one thread per connection, prompt shutdown
  session.*   one connection: handshake, commands, publish (does no I/O itself)
  chunk.*     RTMP chunk stream reader/writer      amf0.*   AMF0       flv.*   FLV / Enhanced RTMP demux
src/media-decoder.*          FFmpeg decoders: packets in, pictures and PCM out, in formats OBS takes directly
src/rtmp-server-source.cpp   the OBS source: properties, ingest thread, obs_source_output_*
src/plugin-main.cpp          module entry point
tools/                       test-publisher (an RTMP client built on FFmpeg), rtmp-probe, obs-harness
tests/                       unit, integration and end-to-end tests
```

The RTMP library can be used on its own: see [`src/rtmp/README.md`](src/rtmp/README.md).

### Tests

```bash
cd build_macos && ctest -C Release --output-on-failure
```

| Test | What it proves |
| --- | --- |
| `amf0`, `chunk`, `flv` | Wire formats: round trips, byte-at-a-time feeding, extended timestamps, malformed and hostile input |
| `session` | A simulated encoder through handshake, connect, publish, media, metadata, acknowledgements, pings, rejections, 32-bit timestamp wrap |
| `hub` | Over real TCP sockets: two keys on one port, busy and unknown keys refused, removing a source kicks its publisher and frees the port, reconnects |
| `fuzz` | Thousands of corrupted conversations and random noise neither crash nor hang a session |
| `e2e` | A real RTMP client (FFmpeg) streams a known pattern; the plugin's decoders check size, picture content and audio level |
| `obs_harness` | The built plugin loaded into the **real libobs of an installed OBS** (headless): the source registers, receives a stream, OBS ends up holding the right picture and audio, a publisher is killed mid-stream and a new one takes over, and the source is destroyed cleanly while streaming |

`e2e` and `obs_harness` need no `ffmpeg` binary: `test-publisher` is built on the FFmpeg libraries that ship with the
OBS dependencies. The harness never starts or modifies the OBS application; it needs OBS installed at
`/Applications/OBS.app` (`-DOBS_APP_PATH=` to change) and is skipped otherwise. The protocol tests also run in CI
on Linux and macOS under sanitizers.

`rtmp-probe` listens the way the source does and prints what arrives, which is handy when a sender misbehaves:
`rtmp-probe --decode --port 1935 --key stream`.

### Following OBS releases

`buildspec.json` pins the OBS sources and the *obs-deps* bundle (which holds FFmpeg) the plugin is built against.
The plugin uses the FFmpeg already loaded in OBS, so **the FFmpeg major version must match your OBS**; it is
currently pinned to OBS 32.2.2 / obs-deps 2026-07-15 (libavcodec 62). When a new OBS changes FFmpeg, copy the
`dependencies` → `prebuilt` entry from that release's `CMakePresets.json` into `buildspec.json`, bump the
`obs-studio` version, delete `.deps/` and rebuild. To see which libavcodec your OBS has:

```bash
otool -L /Applications/OBS.app/Contents/Frameworks/libavcodec.dylib | sed -n 2p
```

## Ideas for later

- Windows and Linux builds
- Hardware decoding (VideoToolbox, D3D11VA, VAAPI)
- Showing the last refused connection in the status line
- A Tools-menu or dock panel
- RTMPS, and the digest handshake for older encoders

## Contributing

Bug reports and pull requests are welcome, particularly reports of which cameras and apps work. Please run the test
suite before sending a change.

## License

The repository carries two licenses, because the OBS plugin has to be GPL while the RTMP library does not:

| Part | License |
| --- | --- |
| `src/rtmp/` (the RTMP server library) and the tests for it | [LGPL-2.1-or-later](src/rtmp/LICENSE) |
| The OBS plugin, the FFmpeg decoding layer, the tools, the build scripts and everything else | [GPL-2.0-or-later](LICENSE) |

The plugin cannot be LGPL as a whole: it links `libobs`, which is GPL-2.0-or-later, and runs against the FFmpeg that
OBS ships, which is built as GPL version 3, and the build scripts come from the GPL-2.0 OBS plugin template. The RTMP
library depends on none of that, so it is released under the more permissive LGPL for reuse in other projects. Every
source file carries an SPDX license identifier.

## Acknowledgements

Built on [OBS Studio](https://obsproject.com/) and its [plugin template](https://github.com/obsproject/obs-plugintemplate),
and decoding with [FFmpeg](https://ffmpeg.org/).
