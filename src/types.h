/* types.h: type re-definitions | re-definiciones de tipos */

#pragma once

#include <stdint.h>
#include <uchar.h>

/* Booleans | Booleanos */
typedef bool bool8; // true / false
typedef int32_t bool32; // 0 is false, otherwise true | 0 es falso, otro valor es verdadero

/* Characters | Caracteres */
typedef char8_t char8; // UTF-8

/* Unsigned integers | Enteros sin signo */
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* Signed integers | Enteros con signo */
typedef int8_t  int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

/* Floating-point numbers | Números de punto flotante */
typedef float float32;
typedef double float64;

static_assert(sizeof(bool8) == 1, "Unexpected bool8 size, check typedef"); // Tamaño inesperado de bool8
static_assert(sizeof(bool32) == 4, "Unexpected bool32 size, check typedef"); // Tamaño inesperado de bool32

static_assert(sizeof(char8) == 1, "Unexpected char8 size, check typedef"); // Tamaño inesperado de char8

static_assert(sizeof(uint8) == 1, "Unexpected uint8 size, check typdef"); // Tamaño inesperado de uint8
static_assert(sizeof(uint16) == 2, "Unexpected uint16 size, check typedef"); // Tamaño inesperado de uint16
static_assert(sizeof(uint32) == 4, "Unexpected uint32 size, check typedef"); // Tamaño inesperado de uint32
static_assert(sizeof(uint64) == 8, "Unexpected uint64 size, check typedef"); // Tamaño inesperado de uint64

static_assert(sizeof(int8) == 1, "Unexpected int8 size, check typedef"); // Tamaño inesperado de uint8
static_assert(sizeof(int16) == 2, "Unexpected int16 size, check typedef"); // Tamaño inesperado de uint16
static_assert(sizeof(int32) == 4, "Unexpected int32 size, check typedef"); // Tamaño inesperado de uint32
static_assert(sizeof(int64) == 8, "Unexpected int64 size, check typedef"); // Tamaño inesperado de uint64

static_assert(sizeof(float32) == 4, "Unexpected float32 size, check typedef"); // Tamaño inesperado de float32
static_assert(sizeof(float64) == 8, "Unexpected float64 size, check typedef"); // Tamaño inesperado de float64

/* 21/11/2025 Luis Arturo Ramos Valencia - kanso engine */
