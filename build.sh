#!/bin/env bash
clang -std=c23 -O0 -g src/main.c -o bin/main -DKSO_DEBUG=1 # -DKSO_VDEBUG=1
