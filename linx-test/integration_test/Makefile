CC=/home/public/block_toolchain/linx64-unknown-elf/bin/linx64-unknown-elf-gcc

all: test std_link test_atomic

std_link: test.c
	$(CC) -O2 $< -o $@

test: test.o start.o
	$(CC) -O2 $^ -nostartfiles -o $@

test.o: test.c
	$(CC) -O2 -c $< -o $@

start.o: start.s
	$(CC) -c $< -o $@

test_atomic: atomic.o start.o atomic_testTmp.o
	$(CC) -O2 $^ -nostartfiles -o $@

atomic.o: atomic/atomic.c
	$(CC) -O2 -c $^ -o $@

atomic_testTmp.o: atomic/atomic_testTmp.c
	$(CC) -O2 -c $^ -o $@

.PHONY: clean
clean:
	rm `find . -name test -o -name test_atomic -o -name std_link -o -name "*.o"`
