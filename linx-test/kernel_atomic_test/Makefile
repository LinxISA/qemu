CC=linx64-linux-gnu-gcc
OBJ=linx64-linux-gnu-objdump
INC:= \
	-I./atomic

all: test_atomic

test_atomic.o: test_atomic.c
	$(CC) --static -g $(INC) -c $< -o $@

test_atomic: test_atomic.o
	$(CC) --static -g $(INC) $^ -o $@
	$(OBJ) -xd $@ > $@.lst

.PHONY: clean
clean:
	rm -f test_atomic *.o *.lst
