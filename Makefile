# Amiga build (amiga-gcc in WSL):  make
# Host test build (native gcc):     make host
CC      = m68k-amigaos-gcc
CFLAGS  = -O2 -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer
SRCS    = src/main.c src/hfs.c src/hfsio.c src/charset.c src/macfmt.c
OUT     = out

all: $(OUT)/MacBrowse $(OUT)/MacBrowse.info $(OUT)/MacHFS-Handler $(OUT)/MacMount

$(OUT)/MacBrowse: $(SRCS) src/gui.c src/mount.c src/*.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $(OUT)/MacBrowse $(SRCS) src/gui.c src/mount.c

$(OUT)/MacMount: src/macmount.c src/mount.c src/hfs.c src/hfsio.c src/charset.c src/*.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DHFS_NOCHECK -o $@ src/macmount.c src/mount.c src/hfs.c src/hfsio.c src/charset.c

host: $(OUT)/macbrowse-host

$(OUT)/macbrowse-host: $(SRCS) src/gui_stub.c src/*.h
	mkdir -p $(OUT)
	gcc -O1 -g -Wall -Wno-pointer-sign -o $@ $(SRCS) src/gui_stub.c

HSRCS   = src/hstart.s src/handler.c src/hlibc.c src/hfs.c src/hfsio.c src/charset.c

handler: $(OUT)/MacHFS-Handler

# A DOS handler gets the startup packet, not a CLI/Workbench start: no libnix startup code.
$(OUT)/MacHFS-Handler: $(HSRCS) src/*.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -nostartfiles -DHFS_NOCHECK -o $@ $(HSRCS)

forktest: $(OUT)/forktest-host $(OUT)/forktest

$(OUT)/forktest-host: test/forktest.c src/hfs.c src/hfsio.c src/charset.c src/*.h
	mkdir -p $(OUT)
	gcc -O1 -g -Wall -Wno-pointer-sign -o $@ test/forktest.c src/hfs.c src/hfsio.c src/charset.c

$(OUT)/forktest: test/forktest.c src/hfs.c src/hfsio.c src/charset.c src/*.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ test/forktest.c src/hfs.c src/hfsio.c src/charset.c

clean:
	rm -rf $(OUT)

icon: $(OUT)/MacBrowse.info

$(OUT)/MacBrowse.info: tools/mkicon.py
	mkdir -p $(OUT)
	python3 tools/mkicon.py $@
