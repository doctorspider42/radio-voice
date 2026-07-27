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
| [ASIO SDK](https://www.steinberg.net/developers/) | Steinberg ASIO SDK Licensing Agreement | no, but **not redistributable** | must be downloaded manually |

Nothing is vendored into this repository. Every dependency is fetched by CMake
at configure time, so the licence position of a checkout is unambiguous.

### VST3 SDK

Only the subset needed for *hosting* is compiled: `pluginterfaces`, `base` and
the hosting half of `public.sdk`. The plugin-side base classes, the sample
plugins, the validator and VSTGUI are all skipped.

Turning the host off entirely with `-DRV_ENABLE_VST3=OFF` removes the SDK from
the build. The application still compiles and runs - with the gate, the
equalizer, the limiter and both audio backends - and only plugin hosting is
unavailable. In that configuration nothing copyleft remains.

### ASIO SDK

The ASIO SDK cannot be redistributed, so it is neither committed here nor
fetched by CMake alongside the other dependencies. `tools/fetch-asio-sdk.cmd`
downloads it into `third_party/asiosdk`, which is git-ignored; running that
script accepts Steinberg's ASIO SDK Licensing Agreement, and a copy of the
agreement is placed next to the sources.

CMake picks the SDK up from there automatically. A build without it simply has
no ASIO backend.

Note that distributing a binary built against the ASIO SDK carries obligations
under that agreement - read it before shipping one.

ASIO is a trademark and software of Steinberg Media Technologies GmbH.

### Fonts

The interface renders with **Segoe UI**, loaded at runtime from the system font
directory. No font file is redistributed with this project.

## The virtual audio driver

The optional kernel-mode driver under [`driver/`](driver/) is part of this
project and is covered by the same GPLv3 licence. It links only against the
Windows Driver Kit headers and import libraries, which are covered by the
Microsoft WDK licence terms and are not redistributed here.
