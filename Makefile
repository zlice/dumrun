CFLAGS ?= -Os -std=c99 -Wall -Wextra -Werror -g

SOURCES=dumrun.c

all: dumrun

dumrun: $(SOURCES)
	$(CC) -I/usr/include/freetype2 $(CFLAGS) -o $@ $(SOURCES) -lm -lX11 -lfreetype -lxkbcommon

.PHONY: clean
clean:
	$(RM) dumrun
