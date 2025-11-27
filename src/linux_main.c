#include "log.c"

#include "defines.h"
#include "types.h"

#include <stdlib.h>
#include <wayland-client.h>

typedef struct {
	struct wl_display* display;
	struct wl_registry* registry;
	struct wl_compositor* compositor;
	struct wl_surfac* surface;
} WaylandServer;

#define MIN_WLCOMPOSITOR_VERSION 4 // 3 if not using HiDPI support, 1 if not needing screen rotation
#define MAX_WLCOMPOSTIOR_VERSION 6
// NOTE(vluis): Versioning of wl_compositor and wl_surface are linked | Las versiones de wl_compositor y wl_surface están vinculadas
#define MIN_WLSURFACE_VERSION MIN_WLCOMPOSITOR_VERSION
#define MAX_WLSURFACE_VERSION MAX_WLCOMPOSITOR_VERSION

/*
 * [EN] wl_registry event global: notifies the client of global objects.
 * [ES] Evento global de wl_registry: notifica al cliente sobre objetos globales
 *
 * [EN] The event notifies the client that a global object with the given name is now available, and
 * it implements the given version of the given interface.
 * [ES] El evento notifica al cliente que un objeto global con el nombre (name) dado, está ahora
 * disponible, el cual implementa la versión (version) dada de la interfaz (interface) dada.
 *
 * @param name numeric name of the global object | nombre numérico del objeto global
 * @param interface interface implemented by the object | interfaz implementada por el objeto
 * @param version interface version | versión de la interfaz
 */
void waylandRegistryEventGlobal(void *data, struct wl_registry *registry, uint32 name, 
		const char *interface, uint32 version)
{
	WaylandServer* server = data;

	logTrace("Available wayland global object: %s v%u", interface, version); // TODO: remove later
}

/*
 * [EN] wl_registry event global_remove: notifies the client of removed global objects.
 * [ES] Evento global_remove de wl_registry : notifica al cliente de objetos globales removidos.
 *
 * [EN] This event notifies the client that the global identified by name is no longer available. If
 * the client bound to the global using the bind request, the client should now destroy that object.
 * [ES] Este evento notifica al cliente que el objeto global identificado por el nombre (name) dado
 * ya no está disponible. Si el cliente se vinculó al objeto global a través de una petición (bind),
 * el cliente ahora debería destruir dicho objeto.
 *
 * @param name numeric name of the global object | nombre numérico del objeto global
 */
void waylandRegistryEventGlobalRemove(void *data, struct wl_registry *registry, uint32 name)
{
	/* Intentionally left blank | Intencionalmente en blanco */
}

global_variable struct wl_registry_listener wayland_listener_registry = {
	.global = waylandRegistryEventGlobal,
	.global_remove = waylandRegistryEventGlobalRemove
};

int32 main(void)
{
	WaylandServer wayland_server = { nullptr };

	wayland_server.display = wl_display_connect(nullptr);
	if (wayland_server.display == nullptr) {
		logFatal("Coldn't connect to a running wayland server. Check if WAYLAND_DISPLAY is set."); // no se pudo establecer conexión a un servidor wayland. Revisar si existe WAYLAND_DISPLAY.
		return EXIT_FAILURE;
	}

	wayland_server.registry = wl_display_get_registry(wayland_server.display);
	if (wayland_server.registry == nullptr) {
		logFatal("Coldn't get the wl_registry object."); // no se pudo obtener el objecto wl_registry
		return EXIT_FAILURE;
	}

	wl_registry_add_listener(wayland_server.registry, &wayland_listener_registry, &wayland_server);

	wl_display_roundtrip(wayland_server.display); // wait for wl_registry events to process | esperar a que se procesen los eventos de wl_registry

	wl_display_disconnect(wayland_server.display);

	return EXIT_SUCCESS; // finalizar con éxito
}
