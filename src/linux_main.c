#include "defines.h"
#include "log.c"
#include "types.h"

#include <stdlib.h>

// NOTE(vluis): needed for wayland client
#include <wayland-client.h>
// #include <stdlib.h>
#include <string.h>
#include "linux/xdg-shell-client-protocol.h"
#include "linux/xdg-shell-protocol.c"

// NOTE(vluis): needed for software-render pixel buffers
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
	struct wl_registry_listener wl_registry;
	struct wl_shm_listener wl_shm;
	struct xdg_wm_base_listener xdg_wm_base;
	struct xdg_surface_listener xdg_surface;
	struct xdg_toplevel_listener xdg_toplevel;
} WaylandListeners;

typedef struct {
	struct wl_display* wl_display;
	struct wl_registry* wl_registry;
	struct wl_compositor* wl_compositor;
	struct wl_shm* wl_shm;
	struct xdg_wm_base* xdg_wm_base;
	WaylandListeners listeners;
} WaylandServerState;

typedef struct {
	struct wl_surface* wl_surface;
	struct xdg_surface* xdg_surface;
	struct xdg_toplevel* xdg_toplevel;
	int32 surface_width;
	int32 surface_height;
} WaylandClientState;

typedef struct {
	WaylandServerState server;
	WaylandClientState client;
} WaylandState;

#define MIN_WLCOMPOSITOR_VERSION 4 // 3 if not using HiDPI support, 1 if not needing screen rotation
#define MAX_WLCOMPOSITOR_VERSION 6
#define MIN_WLSHM_VERSION 2 // TODO(vluis): Research the actual minimal version
#define MAX_WLSHM_VERSION 2
#define MIN_XDGWMBASE_VERSION 5
#define MAX_XDGWMBASE_VERSION 7

#define STD_WIDTH 1280
#define STD_HEIGHT 720
#define BYTES_PER_PXL 4

global_variable bool8 running; // false 

/*
 * [EN] The wayland client tries to bind to a global object (of the wayland server)
 * [ES] El cliente wayland intenta vincularse a un objeto global (del servidor wayland)
 *
 * [EN] This function guarantees the successful binding to the requested global object, otherwise it
 * will abort() the program.
 * [ES] Esta función garantiza el vínculo exitoso al objeto globar requerido, de otra forma abortará
 * (abort()) el programa.
 *
 * @param display wl_display global object | objeto global wl_display
 * @param registry wl_registry global object | objeto global wl_registry
 * @param interface wl_interface of the global object | interfaz (wl_interface) del objecto global
 * @param name numeric name of the global object reported by the server | nombre numérico del objeto global reportado por el servidor
 * @param server_version global object's version implemented by the server | versión del objeto global implementada por el servidor
 * @param min_supported_version minimal supported version by the client | versión mínima soportada por el cliente
 * @param max_supported_version maximal supported version by the client | versión máxima soportada por el cliente
 */
[[nodiscard]] internal void* waylandBindToGlobal(struct wl_display* display,
		struct wl_registry* registry, const struct wl_interface* interface, uint32 name,
		uint32 server_version, uint32 min_supported_version, uint32 max_supported_version)
{
	if (server_version < min_supported_version) {
		logFatal("The version (%u) of the wayland global object %s, is lower than the minimal "
				"supported by kanso (%u).", server_version, interface->name, min_supported_version);
		goto error;
	} else if (server_version > max_supported_version) {
		server_version = max_supported_version;
	}
	void* object = wl_registry_bind(registry, name, interface, server_version);
	return object;

	error:
		wl_display_disconnect(display);
		abort();
}

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
internal void waylandRegistryEventGlobal(void *data, struct wl_registry *registry, uint32 name, 
		const char *interface, uint32 version)
{
	WaylandServerState* server = data;

	// TODO(vluis): explore if (!0) is faster than (0 == 0) in the context of if statements
	if (!strcmp(wl_compositor_interface.name, interface)) {
		server->wl_compositor = waylandBindToGlobal(server->wl_display, server->wl_registry,
				&wl_compositor_interface, name,	version, MIN_WLCOMPOSITOR_VERSION,
				MAX_WLCOMPOSITOR_VERSION);
		logInfo("Successful bind to the wayland global object %s", interface);
	}
	else if (!strcmp(wl_shm_interface.name, interface)) {
		server->wl_shm = waylandBindToGlobal(server->wl_display, server->wl_registry,
				&wl_shm_interface, name, version, MIN_WLSHM_VERSION, MAX_WLSHM_VERSION);
		wl_shm_add_listener(server->wl_shm, &server->listeners.wl_shm, nullptr);
		logInfo("Successful bind to the wayland global object %s", interface);
	}
	else if (!strcmp(xdg_wm_base_interface.name, interface)) {
		server->xdg_wm_base = waylandBindToGlobal(server->wl_display, server->wl_registry,
				&xdg_wm_base_interface, name, version, MIN_XDGWMBASE_VERSION,
				MAX_XDGWMBASE_VERSION);
		xdg_wm_base_add_listener(server->xdg_wm_base, &server->listeners.xdg_wm_base, nullptr);
		logInfo("Successful bind to the wayland global object %s", interface);
	}

	logTrace("Available wayland global object: %s v%u", interface, version); // TODO: remove later
}

/*
 * [EN] wl_registry event global_remove: notifies the client of removed global objects.
 * [ES] Evento global_remove de wl_registry : notifica al cliente de objetos globales removidos.
 *
 * [EN] This event notifies the client that the global identified by name is no longer available. If
 * the client bound to the global using the bind request, the client should now destroy that object.
 * [ES] Este evento notifica al cliente que el objeto global identificado por el nombre (name) dado
 * ya no está disponible. Si el cliente se vinculó al objeto global a través de una petición de
 * vinculación (bind), el cliente ahora debe destruir dicho objeto.
 *
 * @param name numeric name of the global object | nombre numérico del objeto global
 */
internal void waylandRegistryEventGlobalRemove(void *data, struct wl_registry *registry,
		uint32 name)
{
	/* Intentionally left blank | Intencionalmente en blanco */
}

/*
 * [EN] wl_shm event format: informs the client about a valid pixel format that can be used for
 * buffers. Example formats: argb8888 and xrgb8888.
 * [ES] Evento format de wl_shm: informa al cliente sobre un formato válido de píxeles que puede ser
 * usado por 'buffers'. Formatos ejemplo: argb8888 y xrgb8888.
 *
 * @param format buffer pixel format | formato de búfer de píxeles
 */
internal void waylandShmEventFormat(void *data, struct wl_shm* shm, uint32 format)
{
	/* 
	 * [EN] Intentionally left blank since we're using one of the two 'always supported' formats:
	 * argb8888 and xrgb8888.
	 * [ES] Intencionalmente en blanco ya que vamos a utilizar uno de los dos formatos siempre
	 * disponibles: argb8888 y xrgb8888.
	 */
}

/*
 * [EN] xdg_wm_base event ping: asks the client if it's still alive, as response the function sends
 * a "pong" request passing the serial specified in the event back to the compositor.
 * [ES] Evento ping de xdg_wm_base: pregunta al cliente si sigue vivo, como respuesta la función
 * envía una petición "pong" pasando el serial especificado en el evento de regreso al compositor.
 *
 * @param serial pass this to the pong request | pasar ésto a la petición pong
 */
internal void waylandXdgWmBaseEventPing(void *data, struct xdg_wm_base *xdg_wm_base, uint32 serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

/*
 * [EN] xdg_surface event configure: marks the end of a configure sequence. A configure sequence is
 * a set of one or more events configuring the state of the xdg_surface.
 * [ES] Evento configure de xdg_surface: marca el final de una secuencia de configuración. Una
 * secuencia de configuración es un conjunto de uno o más eventos que configuran el estado de una
 * superficie xdg_surface.
 *
 * @param serial serial of the configure event | serial del evento configure
 */
internal void waylandXdgSurfaceEventConfigure(void *data, struct xdg_surface *xdg_surface,
		uint32 serial)
{
	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

	xdg_surface_ack_configure(xdg_surface, serial);

	if (client->surface_width != 0 && client->surface_height != 0) {
		wl_surface_commit(client->wl_surface);
		return;
	}

	/* 
	 * [EN] The following code only gets executed on the first call to the function 
	 * [ES] El siguiente código únicamiente se ejecutará en la primer llamada a la función
	 */
	client->surface_width = STD_WIDTH;
	client->surface_height = STD_HEIGHT;

	/* display buffer allocation */
	int32 bytes_per_row = client->surface_width * BYTES_PER_PXL; // stride (in bytes)
	int32 size = client->surface_width * client->surface_height * BYTES_PER_PXL; // pixel buffer size (in bytes) | tamaño (en bytes) del buffer de píxeles
	char* shm_name = "/wl_kanso";
	int32 fd = shm_open(shm_name, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		logFatal("Failed to open a POSIX shared memory object"); // No se pudo crear un objeto de memoria compartida POSIX
		// TODO(vluis): errno available
	}
	ftruncate(fd, size);
	struct wl_shm_pool* shm_pool = wl_shm_create_pool(server->wl_shm, fd, size);
	struct wl_buffer* wl_buffer = wl_shm_pool_create_buffer(shm_pool, 0, client->surface_width,
			client->surface_height, bytes_per_row, WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(client->wl_surface, wl_buffer, 0, 0);
	xdg_surface_set_window_geometry(client->xdg_surface, 0, 0, client->surface_width,
			client->surface_height); // TODO(vluis) check if x = 0 and y = 0 is correct
	shm_unlink(shm_name);
	close(fd);

	wl_surface_commit(client->wl_surface);
}

/*
 * [EN] xdg_toplevel event configure: asks the client to resize its toplevel surface or to change 
 * its state. The configured state should not be applied immediately.
 *
 * [ES] Evento configure de xdg_toplevel: pide al cliente cambiar el tamaño o el estado de su
 * superficie toplevel. El estado configurado no debe de ser aplicado inmediatamente.
 *
 * @param width suggested width for the client's surface | ancho sugerido para la superficie del cliente
 * @param height suggested height for the client's surface | altura sugerida para la superficie del cliente
 * @param states specifies how the width/height should be interpreted | especifica cómo interpretar 'width'/'height'
 */
internal void waylandXdgToplevelEventConfigure(void *data, struct xdg_toplevel *xdg_toplevel,
		int32 width, int32 height, struct wl_array *states)
{
	/* states:
	 *    1 maximized - since v2
	 *    2 fullscreen - since v2
	 *    3 resizing - since v2
	 *    4 activated - since v2
	 *    5 tiled_left - since v2
	 *    6 tiled_right - since v2
	 *    7 tiled_top - since v2
	 *    8 tiled_bottom - since v2
	 *    9 suspended - since v6
	 *    10 constrained_left - since v7
	 *    11 constrained_right - since v7
	 *    12 constrained_top - since v7
	 *    13 constrained_bottom - since v7
	 */
	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

	if (client->surface_width == 0 && client->surface_height == 0) {
		/* [EN] The server has not called waylandXdgSurfaceEventConfigure() for the first time,
		 * therefore we need to wait until these variables are not zero.
		 * [ES] El servidor no ha llamado a waylandXdgSurfaceEventConfigue() por primera vez, por lo
		 * tanto debemos esperar a que estas variables sean diferentes de cero.
		 */
		return;
	}

	if (width != 0 && height != 0) {
		client->surface_width = width;
		client->surface_height = height;
	}

	if (client->surface_width == 0 && client->surface_height == 0) {
		client->surface_width = STD_WIDTH;
		client->surface_height = STD_HEIGHT;
	}

	/* display buffer allocation */
	int32 bytes_per_row = client->surface_width * BYTES_PER_PXL; // stride (in bytes)
	int32 size = client->surface_width * client->surface_height * BYTES_PER_PXL; // pixel buffer size (in bytes) | tamaño (en bytes) del buffer de píxeles

	char* shm_name = "/wl_kanso";
	int32 fd = shm_open(shm_name, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		logFatal("Failed to open a POSIX shared memory object"); // No se pudo crear un objeto de memoria compartida POSIX
		// TODO(vluis): errno available
	}
	ftruncate(fd, size);
	struct wl_shm_pool* shm_pool = wl_shm_create_pool(server->wl_shm, fd, size);
	struct wl_buffer* wl_buffer = wl_shm_pool_create_buffer(shm_pool, 0, client->surface_width,
			client->surface_height, bytes_per_row, WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(client->wl_surface, wl_buffer, 0, 0);
	xdg_surface_set_window_geometry(client->xdg_surface, 0, 0, client->surface_width,
			client->surface_height); // TODO(vluis) check if x = 0 and y = 0 is correct

	/* initial render */
	// 32-bit RGB format, [31:0] x:R:G:B 8:8:8:8 little endian
	void* pxl_buffer = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int32 row = 0; row < client->surface_height; ++row) {
		uint32* pxl = (uint32*)((uint8*)pxl_buffer + (row * bytes_per_row));
		for (int32 col = 0; col < client->surface_width; ++col) {
			uint8* b = (uint8*)pxl; // padding
			uint8* g = (uint8*)pxl + 1;
			uint8* r = (uint8*)pxl + 2;
			uint8* x = (uint8*)pxl + 3;
			*r = 0;
			*g = row;
			*b = col;
			pxl++;
		}
	}

	munmap(pxl_buffer, size);
	shm_unlink(shm_name);
	close(fd);
}

/*
 * [EN] xdg_toplevel event close: sent by the compositor when the user requests the surface to be
 * closed.
 * [ES] Evento close de xdg_toplevel: eviado por el compositor cuando el usuario pide que la
 * superficie sea cerrada.
 */
internal void waylandXdgToplevelEventClose(void *data, struct xdg_toplevel *xdg_toplevel)
{
	WaylandState* wayland_state = data;
	// TODO(vluis): Send dialog to user to confirm exit, before actually closing

	// TODO(vluis): Make sure the following are not actually needed (no memory leaking on server side by not calling these
	// xdg_toplevel_destroy(client->xdg_toplevel);
	// xdg_surface_destroy(client->xdg_surface);
	// wl_surface_destroy(client->wl_surface);

	running = false;
}

/*
 * [EN] xdg_toplevel event configure_bounds: it may be sent prior to a xdg_toplevel.configure event
 * to communicate the bounds a window geometry size is recommended to constrain to.
 * [ES] Evento configure_bounds de xdg_toplevel: puede ser enviado antes de un evento
 * 'xdg_toplevel.configure' para comunicar los límites recomendados para el tamaño de la ventana.
 *
 * @param width suggested maximum width for the client's surface | ancho máximo sugerido para la superficie del cliente
 * @param height suggested maximum height for the client's surface | altura máxima sugerida para la superficie del cliente
 */
internal void waylandXdgToplevelEventConfigureBounds(void *data, struct xdg_toplevel *xdg_toplevel,
		int32 width, int32 height)
{
	// if width == 0 && height == 0 { ignore the event } // bounds unknown
	// since version 4
	// Intentionally Left Blank, for now
	// TODO(vluis): Maybe consider these before actually setting the width / height?
	// TODO(vluis): Maybe use these to set the renderer output size?
}

/*
 * [EN] xdg_toplevel event wm_capabilities: advertises the capabilities supported by the compositor.
 * If a capability isn't supported, the client should hide or disable the UI elements that expose
 * this functionality. The compositor will ignore requests it doesn't support.
 * [ES] Evento wm_capabilities de xdg_toplevel: anuncia las capacidades soportadas por el
 * compositor. Si una capacidad no es soportada, el cliente debería ocultar o desabilitar los
 * elementos UI (de interfaz de usuario) que exponen dicha funcionalidad. El compositor ignorará las
 * peticiones que no estén soportadas.
 *
 * @param capabilities array of 32-bit capabilities (native endianness) | arreglo de 'capabilities' de 32-bit (en endianness nativo)
 */
void waylandXdgToplevelEventWmCapabilities(void *data, struct xdg_toplevel *xdg_toplevel,
		struct wl_array *capabilities)
{
	/* capabilities:
	 *    1 window_menu 
	 *    2 maximize
	 *    3 fullscreen
	 *    4 minimize
	 */
	// since version 5
	// TODO(vluis): Consider these befor allowing operations like: fullscreen / minimize / ...?
}

int32 main(void)
{
	WaylandState wayland_state = { 0 };
	/* connection to the walyand-server */
	WaylandServerState* wayland_server = &wayland_state.server;

	wayland_server->listeners.wl_registry.global = waylandRegistryEventGlobal;
	wayland_server->listeners.wl_registry.global_remove = waylandRegistryEventGlobalRemove;
	wayland_server->listeners.wl_shm.format = waylandShmEventFormat;
	wayland_server->listeners.xdg_wm_base.ping = waylandXdgWmBaseEventPing;
	wayland_server->listeners.xdg_surface.configure = waylandXdgSurfaceEventConfigure;
	wayland_server->listeners.xdg_toplevel.configure = waylandXdgToplevelEventConfigure;
	wayland_server->listeners.xdg_toplevel.close = waylandXdgToplevelEventClose;
	wayland_server->listeners.xdg_toplevel.configure_bounds =
			waylandXdgToplevelEventConfigureBounds;
	wayland_server->listeners.xdg_toplevel.wm_capabilities = waylandXdgToplevelEventWmCapabilities; // TODO(vluis): test limitting xdg_wm_base version to 4 and see if this throws an error

	wayland_server->wl_display = wl_display_connect(nullptr);
	wayland_server->wl_registry = wl_display_get_registry(wayland_server->wl_display);
	wl_registry_add_listener(wayland_server->wl_registry, &wayland_server->listeners.wl_registry,
			wayland_server);
	wl_display_roundtrip(wayland_server->wl_display); // wait for wl_registry events to process | esperar a que se procesen los eventos de wl_registry


	/* initialization of the wayland-client's surface (window) */
	WaylandClientState* wayland_client = &wayland_state.client;
	wayland_client->wl_surface = wl_compositor_create_surface(wayland_server->wl_compositor);
	wayland_client->xdg_surface = xdg_wm_base_get_xdg_surface(wayland_server->xdg_wm_base,
			wayland_client->wl_surface);
	xdg_surface_add_listener(wayland_client->xdg_surface, &wayland_server->listeners.xdg_surface,
			&wayland_state);
	wayland_client->xdg_toplevel = xdg_surface_get_toplevel(wayland_client->xdg_surface);
	xdg_toplevel_add_listener(wayland_client->xdg_toplevel, &wayland_server->listeners.xdg_toplevel,
			&wayland_state);
	xdg_toplevel_set_title(wayland_client->xdg_toplevel, "kanso");

	wl_surface_commit(wayland_client->wl_surface);

	running = true;

	while (running) {
		wl_display_dispatch(wayland_server->wl_display); // process events | procesar eventos acumulados
		wl_display_flush(wayland_server->wl_display); // send requests | enviar peticiones acumuladas
	}

	wl_display_disconnect(wayland_server->wl_display);

	return EXIT_SUCCESS; // finalizar con éxito
}
