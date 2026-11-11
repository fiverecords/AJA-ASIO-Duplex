# Steinberg ASIO SDK 2.3.4 (three headers)

Only what an ASIO driver compiles against: `common/asio.h` (the types,
error codes, selectors and callbacks), `common/asiosys.h` (platform
macros that asio.h needs) and `common/iasiodrv.h` (the IASIO COM
interface), with `LICENSE.txt`, which those headers point to. They are
copied unchanged.

**Licence.** Since ASIO SDK 2.3.4 Steinberg offers the SDK under its
proprietary ASIO licence or, alternatively, under the GNU General Public
License version 3 (see `LICENSE.txt`). AJA Duplex ASIO uses it under GPLv3,
so the driver DLL, which is built from these headers, is distributed under
GPLv3.

**Provenance.** Copied on 2026-11-08 from
https://github.com/jcelerier/asio-2 at commit
`1bc070d1c7b8f0bf92f9d3b7c72e5880a7dcffdb`, a mirror of the SDK as
Steinberg publishes it (its `changes.txt` lists 2.3.4, "new DUAL
licensing"). Steinberg's own download is at
https://www.steinberg.net/developers/ and has not been compared yet.

SHA-256 of the files as copied:

```
61e56814f24b32216884b2db0b4dcc317111a5ab354d54d19dd9fc13d71b1c7a  LICENSE.txt
6d83d9b6815d21c002b055c628860e8b0a1ca94c5ae1d9d18f137e308ba98096  common/asio.h
387fc21d036c3f39d7f74704bebb1e9cfe0bd8564ccd48088827162b06afe583  common/asiosys.h
5dccca30296a6305c3914882282b9fda6998346580dd95769870b4570a07fe96  common/iasiodrv.h
```

ASIO is a trademark of Steinberg Media Technologies GmbH. The ASIO
logos are not used.
