# Third-party components and licensing

## This project

RadioVoice is licensed under the **GNU General Public License v3.0 or later**
(see [LICENSE](LICENSE)).

That is a direct consequence of hosting VST3 plugins. The Steinberg VST3 SDK is
dual licensed: GPLv3, or a separate proprietary agreement with Steinberg.
Linking the SDK without such an agreement obliges the resulting work to be
GPLv3. If you hold a Steinberg licensing agreement you may relicense this
project's own source under whatever terms that agreement allows - none of the
other dependencies below stand in the way.

## Dependencies

| Component | Licence | Copyleft | How it is obtained |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | no | fetched at configure time |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | no | single header, downloaded at configure time |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | GPLv3 **or** proprietary Steinberg licence | **yes** (under GPLv3) | fetched at configure time, only when `RV_ENABLE_VST3=ON` |
| [ASIO SDK](https://www.steinberg.net/developers/) (host subset) | GPLv3 **or** proprietary Steinberg licence; the `host/` helpers are BSD-3-Clause | **yes** (under GPLv3) | vendored in `third_party/asiosdk` |

Only the ASIO SDK is vendored, and only a nine-file subset of it. Everything
else is fetched by CMake at configure time.

### VST3 SDK

Only the subset needed for *hosting* is compiled: `pluginterfaces`, `base` and
the hosting half of `public.sdk`. The plugin-side base classes, the sample
plugins, the validator and VSTGUI are all skipped.

Turning the host off entirely with `-DRV_ENABLE_VST3=OFF` removes the SDK from
the build. The application still compiles and runs - with the gate, the
equalizer, the limiter and both audio backends - and only plugin hosting is
unavailable. In that configuration nothing copyleft remains.

### ASIO SDK

As of **SDK 2.3.4** the ASIO SDK is dual licensed: the proprietary Steinberg
ASIO License, **or** GPLv3. Earlier releases were proprietary-only and could not
be redistributed at all, which is where the widespread "you must download the
ASIO SDK yourself" advice comes from - it is no longer true.

This project takes the GPLv3 option, which it is already under because of the
VST3 SDK, so the sources are vendored in `third_party/asiosdk` and the build
needs no download step.

What is vendored is deliberately a subset - the nine files a *host* needs:

| Path | Licence |
|---|---|
| `common/asio.h`, `asio.cpp`, `asiosys.h`, `iasiodrv.h` | Steinberg **or** GPLv3 |
| `host/asiodrivers.{h,cpp}`, `ginclude.h` | BSD-3-Clause |
| `host/pc/asiolist.{h,cpp}` | BSD-3-Clause |

Left out on purpose:

- the driver-side sources (`common/combase.*`, `dllentry.cpp`, `wxdebug.h`),
  which carry a 1990s Microsoft copyright with no clear grant, and which a host
  never compiles;
- the **ASIO logo artwork** and the usage-guideline PDFs. Those are trademark
  material, and trademark is not touched by either licence option. The name is
  used here only to say what the software is compatible with.

**ASIO is a trademark and software of Steinberg Media Technologies GmbH.**

If you intend to ship a binary under the *proprietary* option instead, you need
a License Agreement signed by Steinberg - see `third_party/asiosdk/LICENSE.txt`.

`tools/fetch-asio-sdk.cmd` re-downloads the full SDK, for updating to a newer
release; it is not needed for an ordinary build.

ASIO is a trademark and software of Steinberg Media Technologies GmbH.

### Fonts

The interface renders with **Segoe UI**, loaded at runtime from the system font
directory. No font file is redistributed with this project.

## The virtual audio driver

The optional kernel-mode driver under [`driver/`](driver/) is part of this
project and is covered by the same GPLv3 licence. It links only against the
Windows Driver Kit headers and import libraries, which are covered by the
Microsoft WDK licence terms and are not redistributed here.
