#!/bin/env bash
clang -std=c23 -O0 -g src/linux_main.c -o bin/linux_main -lwayland-client -DKSO_DEBUG=1 # -DKSO_VDEBUG=1
