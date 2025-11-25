#include "defines.h"
#include "types.h"
#include <stdlib.h>
#include <stdio.h>

/* logging */

typedef enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } LogType;
/* LogType descriptions | descripciones de LogType
 * LOG_TRACE: Verbose or highly frequent debugging messages | Mensajes con fines de debugging verbosos o muy frecuentes
 * LOG_DEBUG: Debugging messages | Mensajes de debugging
 * LOG_INFO: Information of errorless events | Información de eventos sin errores
 * LOG_WARN: Warning of a non-critical error | Advertencia de un error no crítico
 * LOG_ERROR: Critical but non-terminating error | Error crítico pero sin finalizar la ejecución
 * LOG_FATAL: Critical and unexpected terminating error | Error crítico inesperado que finaliza la ejecución
 */

char* log_type_tags[] = { "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL" };

void logMessage(LogType message_type, const char* message)
{
	FILE* output; // salida destino del mensaje
	if (message_type <= LOG_INFO) {
		output = stdout;
	} else {
		output = stderr;
	}
	fprintf(output, "%s: %s\n", log_type_tags[message_type], message);
}

int32 main(void)
{
	logMessage(LOG_TRACE, "Mensaje Trace");
	logMessage(LOG_DEBUG, "Mensaje Debug");
	logMessage(LOG_INFO, "Mensaje Info");
	logMessage(LOG_WARN, "Mensaje Warning");
	logMessage(LOG_ERROR, "Mensaje Error");
	logMessage(LOG_FATAL, "Mensaje Fatal");

	return EXIT_SUCCESS; // Finalizar con éxito
}

