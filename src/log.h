/* log.h: logging functions declarations */

#pragma once

/* LogType descriptions | descripciones de LogType
 * LOG_TRACE: Verbose or highly frequent debugging messages | Mensajes con fines de debugging verbosos o muy frecuentes
 * LOG_DEBUG: Debugging messages | Mensajes de debugging
 * LOG_INFO: Information of error-less events | Información de eventos sin errores
 * LOG_WARN: Warning of a non-critical error | Advertencia de un error no crítico
 * LOG_ERROR: Critical but non-terminating error | Error crítico pero sin finalizar la ejecución
 * LOG_FATAL: Critical and unexpected terminating error | Error crítico inesperado que finaliza la ejecución
 */
typedef enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } LogType;

#ifdef KSO_DEBUG
	#define logTrace(message, ...)
	#define logDebug(message, ...) logMessage(LOG_DEBUG, message __VA_OPT__(,) __VA_ARGS__)
	#define logInfo(message, ...) logMessage(LOG_INFO, message __VA_OPT__(,) __VA_ARGS__)
	#define logWarn(message, ...) logMessage(LOG_WARN, message __VA_OPT__(,) __VA_ARGS__)
#elifdef KSO_VDEBUG // verbose debug
	#define logTrace(message, ...) logMessage(LOG_TRACE, message __VA_OPT__(,) __VA_ARGS__)
	#define logDebug(message, ...) logMessage(LOG_DEBUG, message __VA_OPT__(,) __VA_ARGS__)
	#define logInfo(message, ...) logMessage(LOG_INFO, message __VA_OPT__(,) __VA_ARGS__)
	#define logWarn(message, ...) logMessage(LOG_WARN, message __VA_OPT__(,) __VA_ARGS__)
#else
	#define logTrace(message, ...)
	#define logDebug(message, ...)
	#define logInfo(message, ...)
	#define logWarn(message, ...)
#endif

void logMessage(LogType message_type, const char* message, ...);

#define logError(message, ...) logMessage(LOG_ERROR, message __VA_OPT__(,) __VA_ARGS__)
#define logFatal(message, ...) logMessage(LOG_FATAL, message __VA_OPT__(,) __VA_ARGS__)

#if defined(KSO_LOG_IMPLEMENTATION)

#include <stdarg.h>
#include <stdio.h>

char* logtype_tags[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" }; // etiquetas

void logMessage(LogType message_type, const char* message, ...)
{
	FILE* output; // salida destino del mensaje
	if (message_type <= LOG_INFO) { // mensajes informativos
		output = stdout; // a salida estándar
	} else { // mensajes de gravedad
		output = stderr; // a salida de error estándar
	}
	fprintf(output, "%s: ", logtype_tags[message_type]); // print tag | imprimir etiqueta
	va_list arguments;
	va_start(arguments, message);
	vfprintf(output, message, arguments);
	va_end(arguments);
	fprintf(output, "%c", '\n');
}

#endif // KSO_LOG_IMPLEMENTATION

/* 25/11/2025 Luis Arturo Ramos Valencia - kanso engine */
