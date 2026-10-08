# scx_soup - standalone sched_ext scheduler
#
# Builds the BPF object with clang + bpftool, generates the skeleton,
# compiles the libbpf userspace loader. Does NOT require libscx or meson.

CLANG      ?= clang
BPFTOOL    ?= bpftool
PKG_CONFIG ?= pkg-config
CC         ?= cc

LIBBPF_CFLAGS := $(shell $(PKG_CONFIG) --cflags libbpf 2>/dev/null)
LIBBPF_LIBS   := $(shell $(PKG_CONFIG) --libs libbpf 2>/dev/null)

VMLINUX   := /sys/kernel/btf/vmlinux
BPF_CFLAGS := -g -O2 -Wall -Werror -target bpf -D__TARGET_ARCH_x86 \
              -Ithird_party/scx/include -Ithird_party/scx/include/scx \
              -Ithird_party/scx -I. \
              -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
              -Wno-missing-declarations
BPF_CFLAGS += -mlittle-endian

ALL: scx_soup

scx_soup.bpf.o: scx_soup.bpf.c \
		third_party/scx/include/scx/common.bpf.h \
		third_party/scx/include/scx/compat.bpf.h \
		third_party/scx/include/scx/cid.bpf.h \
		vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

scx_soup.skel.h: scx_soup.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

vmlinux.h: $(VMLINUX)
	$(BPFTOOL) btf dump file $(VMLINUX) format c > $@

scx_soup: main.c scx_soup.skel.h scx_soup.bpf.o
	$(CC) $(LIBBPF_CFLAGS) -O2 -Wall -o $@ main.c $(LIBBPF_LIBS) -lelf -lz

clean:
	rm -f scx_soup scx_soup.bpf.o scx_soup.skel.h vmlinux.h

install: scx_soup
	install -Dm755 scx_soup /usr/local/bin/scx_soup
	install -Dm644 soup.service /usr/lib/systemd/system/soup.service
	systemctl daemon-reload

.PHONY: ALL clean install