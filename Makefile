DEBUG_FLAGS	 = -g3 -ggdb

CFLAGS = -O0 \
		$(DEBUG_FLAGS) \
		-std=gnu99 \
		-Wall -pedantic -Wextra -Wshadow

all: test

test : sdcard_sim.c asyncfatfs.c test.c

clean :
	rm test