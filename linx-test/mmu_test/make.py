#!/usr/bin/python3
import os
cpath = "/home/huanghaofu/V150B100/"
gcc = "linx64-linux-gnu/bin/linx64-linux-gnu-gcc -static -c "
filepath = "./"
filelist = []

os.system("rm *.elf")
os.system(cpath + gcc + "start.S -o start.o")
os.system(cpath + gcc + "trap.S -o trap.o")
os.system(cpath + gcc + "put.S -o put.o")
os.system(cpath + gcc + "init.c -o init.o")

filelist = os.listdir(filepath)
for each in filelist:
    if each[:3] == "LPT" or each[:3] == "QPT":
        suffix = each.split(".")
        os.system(cpath + gcc + each + " -o " + suffix[0] + ".o")
        os.system(cpath + "linx64-linux-gnu/bin/linx64-linux-gnu-ld -T link.lds -static start.o put.o trap.o init.o " + suffix[0] + ".o -o " + suffix[0] + ".elf")

os.system("rm *.o")
