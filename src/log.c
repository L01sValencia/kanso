/* log.c: logging system */

#include "log.h"
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

/* 25/11/2025 Luis Arturo Ramos Valencia - kanso engine */
