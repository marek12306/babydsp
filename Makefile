# babydsp ALSA plugin for the Logitech Squeezebox Radio.
#
#   make                 plugin for the radio (needs the cross toolchain)
#   make test            host unit test (needs the host's ALSA headers)
#
# The radio's userspace is SqueezeOS glibc 2.11.1 with alsa-lib 1.0.18. The
# plugin is built with an ARM EABI glibc toolchain (any glibc version: the
# functions it calls are all GLIBC_2.4) against alsa-lib 1.0.18
# (alsa-lib.sh). Default prefix: Bootlin's armv5-eabi glibc toolchain.
CROSS_COMPILE ?= arm-linux-
SYSROOT ?= sysroot
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS ?= -O2 -march=armv5te -mtune=arm926ej-s
HOSTCC ?= cc
HOSTCFLAGS ?= -O2 -Wall

PLUGIN := libasound_module_pcm_babydsp.so

# -DPIC: ALSA's SND_PCM_PLUGIN_SYMBOL then emits the version symbol that
# libasound looks up when it loads the plugin (as libtool would pass).
$(PLUGIN): babydsp.c $(SYSROOT)/lib/libasound.so
	$(CC) $(CFLAGS) -Wall -fPIC -DPIC -I$(SYSROOT)/include -shared \
		-Wl,-soname,$(PLUGIN).0 -o $@ babydsp.c -L$(SYSROOT)/lib -lasound
	$(STRIP) --strip-unneeded $@

$(SYSROOT)/lib/libasound.so:
	./alsa-lib.sh $(CROSS_COMPILE) $(SYSROOT)

test: test_state
	./test_state

test_state: test_state.c babydsp.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ test_state.c -lasound

# Processes a 16-bit stereo WAV file: ./test_dsp in.wav out.wav
test_dsp: test_dsp.c babydsp.c
	$(HOSTCC) $(HOSTCFLAGS) -o $@ test_dsp.c -lasound

clean:
	rm -f $(PLUGIN) test_state test_dsp

distclean: clean
	rm -rf $(SYSROOT) build-alsa-lib

.PHONY: test clean distclean
