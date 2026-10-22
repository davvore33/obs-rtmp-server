# rtmp: a small RTMP ingest server library

Plain C++17 and sockets. It depends on neither OBS nor FFmpeg, so you can use it
on its own. It accepts RTMP *publishers* (encoders) and hands you their demuxed
video and audio packets; it does not decode anything and it does not serve
playback.

Licensed under the **LGPL-2.1-or-later** (see `LICENSE` in this directory),
unlike the OBS plugin around it, which is GPL-2.0-or-later.

```cpp
#include "rtmp/hub.h"

class MySink : public rtmp::StreamSink {
public:
    void on_video(const rtmp::VideoPacket &p) override { /* H.264/HEVC/AV1/VP9 access unit */ }
    void on_audio(const rtmp::AudioPacket &p) override { /* AAC/MP3/Opus frame */ }
};

rtmp::Hub::RouteSpec route;
route.bind_address = "127.0.0.1";
route.port = 1935;
route.app = "live";
route.key = "mykey";   // empty accepts any key under "live"

std::string error;
auto registration = rtmp::Hub::instance().add_route(route, std::make_shared<MySink>(), error);
// Streams published to rtmp://127.0.0.1/live/mykey now arrive in MySink.
// Destroy `registration` to remove the route (the port closes with the last one).
```

Packet data is only valid for the duration of the callback. Callbacks run on the
connection's network thread. `tools/rtmp-probe.cpp` is a complete example.

| File | Role |
| --- | --- |
| `hub.*` | Shares ports between routes; one publisher per `(app, key)` |
| `server.*` | TCP listener, one thread per connection, prompt shutdown |
| `session.*` | One connection: handshake, commands, publish (no I/O of its own) |
| `chunk.*` | RTMP chunk stream reader and writer |
| `amf0.*` | AMF0 encoding and decoding |
| `flv.*` | FLV tag and Enhanced RTMP demuxing |
| `socket.*` | Thin cross-platform socket layer |

Everything from the network is treated as hostile: sizes, counts and nesting are
bounded. The tests in `tests/` (amf0, chunk, flv, session, hub, fuzz) are part of
this library.
