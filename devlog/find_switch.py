#!/usr/bin/python3

'''做一个qemu日志过滤器，发现：
   1. 有一个线程在块内被打断
   2. 中间发生一次切换
   3. 线程被恢复
'''

import sys
import re

if len(sys.argv)!=2:
    sys.stderr.write("usage: " + sys.argv[0] + " <log_file>\n")
    exit(0)

cs_out = re.compile(r'[^"]+CS_OUT[^"]+Priv\(1=>1\)')
cs_in = re.compile(r'[^"]+CS_IN[^"]+Priv\(1=>1\)')
evld1 = re.compile(r'  BSTATE.VLD/EN: 1')
evld0 = re.compile(r'  BSTATE.VLD/EN: 0')
trace = re.compile(r'Trace')
oscs =  re.compile(r'linx_debug\(101, 0x0\) info: switch from')

state = 0
lineno = 1
cs_out_line = ""
cs_out_lineno = 0

def parse_one_line(line):
    global state;
    global lineno;
    global cs_out_line;

    if state == 0:
        if cs_out.match(line):
            state = 1
            cs_out_lineno = lineno
            cs_out_line = line
    elif state == 1:
        # 找VLD=1
        if evld1.match(line):
            state = 2
            sys.stdout.write("switch out: " + cs_out_line)
        elif evld0.match(line):
            state = 0
        elif cs_out.match(line):
            print("nest switch out?");
            print("first ", cs_out_lineno, ": ", cs_out_line)
            print("second", lineno, ": ", line)
            state = 0
        elif trace.match(line):
            state = 0
        elif oscs.match(line):
            state = 0
    elif state == 2:
         # 找切换
        if oscs.match(line):
            print("got cs line in ", cs_out_lineno, ", switch line in ", lineno)
        elif cs_in.match(line):
            sys.stdout.write("switch back: " + line)
            state = 0
    else:
        assert(False)

    lineno = lineno+1

with open(sys.argv[1]) as infile:
    for line in infile:
        parse_one_line(line)

print("not found")
