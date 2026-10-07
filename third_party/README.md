# Third-party code

Compiled into the executable (no shared libraries, nothing to install). Each keeps its own
license file, which a binary distribution must include.

| Library | Version | Upstream | Used for |
|---|---|---|---|
| miniupnpc | 2.3.3 (commit b2b496a) | https://github.com/miniupnp/miniupnp | UPnP IGD port mapping for link play (runtime/net.c) |
| libnatpmp | 20120821 (commit 134fc89) | https://github.com/miniupnp/libnatpmp | NAT-PMP port mapping for link play |

Only the library sources are copied (no tests, tools or bindings), unmodified; miniupnpc's
`miniupnpcstrings.h`, which its build generates, is written by hand. libnatpmp's
`wingettimeofday.c`/`.h` are compiled only in the Windows build (`make WIN=1`).
