# babydsp

ALSA external PCM plugin (`libasound_module_pcm_babydsp.so`) used by the
Logitech Squeezebox Radio. SqueezePlay plays through it; it sums the stereo
stream to mono, equalises it and splits it into a tweeter and a woofer signal:

```
L+R -> BEQ 1 -> BEQ 2 -> bass 1 -> bass 2 -+-> tweeter -> left output
                                           `-> woofer  -> right output
```

- Six Q30 Direct Form I biquads, 64-bit accumulation, rounding and a
  ±18500 output clip. The tweeter/woofer crossover is at about 2 kHz at
  44.1 kHz; the plugin does not restrict the sample rate.
- S16, two channels in and out.
- An optional ALSA control (`control { name "Crossover" card "baby" }`) is
  created with value 1; when it is 0 the input is copied unchanged.

This is a reconstruction of the plugin shipped in SqueezeOS 7.x. It behaves
like the original except that every PCM instance keeps its own filter history
and the extplug `init` callback resets it (the original shared one global
history between all instances in a process).

## Files

| File | Purpose |
|---|---|
| `babydsp.c` | the plugin |
| `asound.conf` | the SqueezeOS ALSA configuration that uses it |
| `alsa-lib.sh` | cross-builds alsa-lib 1.0.18 into `sysroot/` for headers and linking |
| `test_state.c` | host test: per-instance state and reset by `init` |
| `test_dsp.c` | host tool: runs a 16-bit stereo WAV file through the filters |

## Building

The radio runs SqueezeOS userspace: glibc and alsa-lib 1.0.18. The plugin is
therefore built with an ARM glibc toolchain whose libc is not newer than the
radio's (the result may only need `GLIBC_2.4` symbols) and against alsa-lib
1.0.18.

The original was built with **Sourcery G++ Lite 2010q1-202 for ARM GNU/Linux**
(`arm-none-linux-gnueabi`, GCC 4.4.1, upstream file
`arm-2010q1-202-arm-none-linux-gnueabi-i686-pc-linux-gnu.tar.bz2`; no longer
hosted by Mentor). Its host binaries are 32-bit x86; on 64-bit Debian or Ubuntu
install `libc6-i386 lib32z1` first.

```sh
export PATH=/path/to/arm-2010q1/bin:$PATH
make
```

`make` downloads alsa-lib 1.0.18 (SHA-256 checked), cross-builds it with the
SqueezeOS configure options into `sysroot/`, then builds and strips
`libasound_module_pcm_babydsp.so`. Another toolchain prefix or sysroot:
`make CROSS_COMPILE=arm-linux-gnueabi- SYSROOT=/path/to/alsa`.

Check the result:

```sh
readelf -V libasound_module_pcm_babydsp.so | grep -o 'GLIBC_[0-9.]*' | sort -u   # GLIBC_2.4
readelf -W --dyn-syms libasound_module_pcm_babydsp.so | grep babydsp
# _snd_pcm_babydsp_open and __snd_pcm_babydsp_open_dlsym_pcm_001
```

The second symbol is the plugin version that libasound checks when it loads
the plugin; it is only emitted with `-DPIC`, which the Makefile passes.

Host test (needs the host's ALSA development files, e.g. `libasound2-dev`):

```sh
make test
```

## Installing

Copy the plugin to `/usr/lib/alsa-lib/libasound_module_pcm_babydsp.so` on the
radio. `asound.conf` (`/etc/asound.conf`) defines `pcm.babydsp` on the `baby`
card and makes it the default device.

To try a build without replacing the installed plugin, point the plugin type
at it in a private configuration:

```sh
cat /usr/share/alsa/alsa.conf - > /tmp/test.conf <<'EOF'
pcm_type.babydsp { lib "/tmp/libasound_module_pcm_babydsp.so" }
pcm.test { type babydsp slave.pcm "hw:0,0" }
EOF
ALSA_CONFIG_PATH=/tmp/test.conf aplay -D test file.wav
```

## License

LGPL-2.1-or-later ([LICENSE](LICENSE)). See [NOTICE](NOTICE) for the origin
of the code and what the licence does not cover.
