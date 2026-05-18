import random
import re

UINT8_MAX = 0xff
UINT16_MAX = 0xffff
UINT32_MAX = 0xffffffff
UINT64_MAX = 0xffffffffffffffff
INT32_MIN = -(1<<31)
INT32_MAX = (1<<31) - 1
INT64_MIN = -(1<<63)
INT64_MAX = (1<<63) - 1


RAND8 = lambda t: random.randint(-0x80, 0x7f)
RAND16 = lambda t: random.randint(-0x8000, 0x7fff)
RAND32 = lambda t: random.randint(-0x80000000, 0x7fffffff)
RAND64 = lambda t: random.randint(-0x8000000000000000, 0x7fffffffffffffff)

URAND8 = lambda t: random.randint(0, 0xff)
URAND16 = lambda t: random.randint(0, 0xffff)
URAND32 = lambda t: random.randint(0, 0xffffffff)
URAND64 = lambda t: random.randint(0, 0xffffffffffffffff)

RAND = lambda l, r: lambda t: random.randint\
(eval(l) if type(l) == str else l, eval(r) if type(r) == str else r)

class Bkgen:

    def __init__(self, commutative=False):
        self.__cnt = 0
        self.__commutative = commutative

    def __print(self, line):
        print(" " if self.__cnt == 0 else ",", end='')
        fmt = "0x{:x}"
        for t in range(len(line)):
            print(fmt.format(line[t] \
            if line[t] >= 0 else (-line[t] ^ UINT64_MAX) + 1), end='')
            if t == 0:
                fmt = ", " + fmt
        print()
        self.__cnt += 1

    def comment(self, comment):
        print("  /*", comment, "*/")

    def addRule(self, num: int, fun):
        if num < 1 or len(fun) == 0:
            return
        for t in range(num):
            baka = []
            for f in fun:
                if type(f) == int:
                    baka.append(f)
                elif callable(f):
                    baka.append(f(baka))
                elif type(f) == str:
                    if re.match("^[_a-zA-Z][_a-zA-Z0-9]*$", f) and callable(eval(f)):
                        baka.append(eval(f)(baka))
                    else:
                        baka.append(eval("lambda t: " + f)(baka))
            if self.__commutative and (self.__cnt & 1) == 1:
                baka[0], baka[1] = baka[1], baka[0]
            self.__print(baka)

    def addData(self, data):
        self.__print(data)

    def complete(self):
        print("\n#define TEST_SIZE ", self.__cnt)
