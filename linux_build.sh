#!/bin/env bash
mkdir -p ./src/linux/
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml ./src/linux/xdg-shell-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml ./src/linux/xdg-shell-protocol.c

clang -std=c23 -O0 -g src/linux_main.c -o bin/linux_main -lc -lwayland-client -D_POSIX_C_SOURCE=200809L -DKSO_DEBUG=1 # -DKSO_VDEBUG=1
