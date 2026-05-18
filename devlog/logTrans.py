#!/usr/bin/python3
import sys

logf = open(sys.argv[1], 'r')
labf = open(sys.argv[2], 'r')
outf = open('qemu_trans.log', 'w')
addr_label_dict = dict()
for line in labf:
    linelist = line.split()
    addr = int(linelist[0].strip(), 16)

    label = linelist[2].strip()
    addr_label_dict[addr] = label
str_buff = [' ', ' ', ' ', ' ', ' ']
i = 0
mark = 0
apd = ''
for line in logf:
    if 'IN:' in line:
        i = 0
        mark = True
    if mark == True:
        str_buff[i] = line
        if i == 2:
            linelist = line.split(':')
            linelist2 = line.split()
            if len(linelist2) > 7:
                next = int(linelist2[6].split(':')[1].split(',')[0], 16)
                if next > 0x80200000 and next < 0xffffffff80000000:
                    next = next - 0x200000 + 0xffffffff00000000
                if next in addr_label_dict:
                    str_buff[1] = str_buff[1].strip() + '  Next:  ' + addr_label_dict[next] + '\n'
            if 'body' not in line:
                addr = int(linelist[0].strip(), 16)

            if addr > 0x80200000 and addr < 0xffffffff80000000:
                addr = addr - 0x200000 + 0xffffffff00000000
            if addr < 0x80200000:
                apd = '\n'
            if addr in addr_label_dict:
                apd = addr_label_dict[addr] + '\n'
            else:
                apd = '\n'
#                print('addr:0x%x %s ' % (addr, linelist[0].strip()))

            str_buff[0] = str_buff[0].strip() + apd

            outf.write(str_buff[0])
            outf.write(str_buff[1])
            outf.write(str_buff[2])
            mark = False
        i = i + 1
    else:
        outf.write(line)
outf.close()
logf.close()
labf.close()


