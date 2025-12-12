/* defines.h: common definitions | definiciones comúnes */

#pragma once
#include "log.h"

#define global_variable static // global-scope variables | variables con alcance global
#define persist static // local-scope variables | variables con alcance local
#define internal static // confine functions to its translation unit | confinar funciones a su unidad de traducción

// TODO(vluis): Is it better to define our assert with other name like ASSERT or KSO_ASSERT?
#if defined(KSO_DEBUG) || defined(KSO_VDEBUG)
	#define assert(condition, message) if (!(condition)) { logFatal(message); abort(); }
#else
	#define assert(condition, message)
#endif

/* 24/11/2025 Luis Arturo Ramos Valencia - kanso engine */
