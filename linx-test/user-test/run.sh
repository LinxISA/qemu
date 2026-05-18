#!/bin/sh

../../build/qemu-linx -d in_asm,exec,cpu,nochain -D log ./a.out
