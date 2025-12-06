#include <stdlib.h>

#include "defines.h"
#include "log.c"
#include "types.h"

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
#include <time.h>

#define STD_WIDTH 1280
#define STD_HEIGHT 720
#define BYTES_PER_PXL 4
#define NUMBER_OF_BUFFERS 3

typedef struct {
	struct wl_registry_listener wl_registry;
	struct wl_shm_listener wl_shm;
	struct xdg_wm_base_listener xdg_wm_base;
	struct xdg_surface_listener xdg_surface;
	struct xdg_toplevel_listener xdg_toplevel;
	struct wl_callback_listener wl_surface_frame_listener;
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
	int32 width;
	int32 height;
	int32 bytes_per_row; // stride
	int32 size;
	int32 fd;
	struct wl_shm_pool* wl_shm_pool;
	struct wl_buffer* wl_buffer;
} WaylandBuffer;

typedef struct {
	struct wl_surface* wl_surface;
	struct xdg_surface* xdg_surface;
	struct xdg_toplevel* xdg_toplevel;
	struct wl_callback* wl_surface_frame;
	WaylandBuffer buffers[NUMBER_OF_BUFFERS];
	int32 latest_buffer_index; // ready to be shown on screen | listo para presentarse en pantalla
	int32 active_buffer_index;
} WaylandClientState;

typedef struct {
	WaylandServerState server;
	WaylandClientState client;
} WaylandState;

[[nodiscard]] bool8 waylandSetUpBuffer(WaylandBuffer* buffer, int32 width, int32 height,
		struct wl_shm* wl_shm)
{
	if (width == 0 || height == 0) {
		width = STD_WIDTH;
		height = STD_HEIGHT;
	}
	buffer->width = width;
	buffer->height = height;
	buffer->bytes_per_row = width * BYTES_PER_PXL; // stride (in bytes)
	buffer->size = width * height * BYTES_PER_PXL; // pixel buffer size (in bytes) | tamaño (en bytes) del buffer de píxeles

	int32 retries = 100;
	if (buffer->fd >= 0) {
		close(buffer->fd);
	}

	int32 fd;
	char shm_name[] = "/kanso_shm_XXXX";
	do {
		char* name = shm_name;
		name = "/kanso_shm_XXXX";
		struct timespec time;
		if (clock_gettime(CLOCK_REALTIME, &time) < 0) {
			logFatal("Couldn't get time from clock_gettime.");
			// TODO(vluis): errno available, print description
			abort();
		}
		uint64 random = time.tv_nsec;
		for (char* c = shm_name; *c != '\0'; ++c) {
			if (*c == 'X') {
				*c = 'A' + (0b0010'0000 & random) + (0b0000'1111 & random); // A-P o a-p
				random >>= 4;
			}
		}
		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		retries--;
	} while (retries > 0 && fd < 0);
	if (fd < 0) {
		logFatal("Failed to open a POSIX shared memory object for pixel buffer allocation."); // No se pudo crear un objeto de memoria compartida POSIX para alojar buffer de píxeles
		// TODO(vluis): errno available, print description
		return false;
	}
	buffer->fd = fd;
	shm_unlink(shm_name);
	ftruncate(buffer->fd, buffer->size);
	if (buffer->wl_shm_pool) {
		wl_shm_pool_destroy(buffer->wl_shm_pool);
	}
	buffer->wl_shm_pool = wl_shm_create_pool(wl_shm, buffer->fd, buffer->size);
	return true;
}

#define MIN_WLCOMPOSITOR_VERSION 4 // 3 if not using HiDPI support, 1 if not needing screen rotation
#define MAX_WLCOMPOSITOR_VERSION 6
#define MIN_WLSHM_VERSION 2 // TODO(vluis): Research the actual minimal version
#define MAX_WLSHM_VERSION 2
#define MIN_XDGWMBASE_VERSION 5
#define MAX_XDGWMBASE_VERSION 7

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
	persist bool8 first_call_done = false;

	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

	xdg_surface_ack_configure(xdg_surface, serial);

	if (first_call_done) {
		wl_surface_commit(client->wl_surface);
		return;
	}

	// [EN] The following code only gets executed on the first call to the function 
	// [ES] El siguiente código únicamiente se ejecutará en la primer llamada a la función
	client->active_buffer_index = 0;
	WaylandBuffer* buffer = &client->buffers[client->active_buffer_index];
	xdg_surface_set_window_geometry(client->xdg_surface, 0, 0, buffer->width, buffer->height);
	buffer->wl_buffer = wl_shm_pool_create_buffer(buffer->wl_shm_pool, 0, buffer->width,
			buffer->height, buffer->bytes_per_row, WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_commit(client->wl_surface);

	first_call_done = true;
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

	if (width == 0 || height == 0) {
		width = STD_WIDTH;
		height = STD_HEIGHT;
	}

	for (int32 i = 0; i < NUMBER_OF_BUFFERS; ++i) {
		bool8 succeded = waylandSetUpBuffer(&client->buffers[i], width, height, server->wl_shm);
		if (!succeded) {
			logFatal("Failed to set up buffers for wayland.");
			abort();
		}
	}

	xdg_surface_set_window_geometry(client->xdg_surface, 0, 0, width, height);

	/*
	 * [EN] NOTE(vluis): In a real-time application (like this one) we can avoid repaint and assign
	 * a new buffer to the wl_surface here, because we're forcing full-size repaints (frames)
	 * continuously.
	 * [ES] En una aplicación en tiempo real (como ésta) podemos evitar repintar y asignar un nuevo
	 * buffer a la superficie 'wl_surface' aquí, ya que estamos forzando repintados de tamaño
	 * completo (nuevos fotogramas) continuamente.
	 */
	
	client->active_buffer_index = -1; // no buffer is active (attached to a surface) | ningún buffer está activo (anclado a una superficie)
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

/*
 * [EN] wl_surface callback event frame: notify that the client should start drawing a new frame.
 * [ES] Evento frame de wl_surface callback: notifica que el cliente debería empezar a dibujar un
 * nuevo fotograma.
 *
 * @param current_time current time in milliseconds, with an undefined base | tiempo actual en milisegundos, sin una base definida
 */
void waylandSurfaceEventNewFrame(void *data, struct wl_callback *callback, uint32 current_time)
{	
	// TODO(vluis): use the previous and the current_time to estimate and log a framerate
	/* initial render */

	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

	int32 render_buffer_index;
	if (client->active_buffer_index < 0) {
		render_buffer_index = 0;
	} else {
		render_buffer_index = (client->active_buffer_index + 1) % NUMBER_OF_BUFFERS;
	}

	WaylandBuffer* buffer = &client->buffers[render_buffer_index];

	void* pxl_buffer = mmap(nullptr, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd,
			0);

	persist int32 offset = 0;
	for (int32 row = 0; row < buffer->height; ++row) {
		uint32* pxl = (uint32*)((uint8*)pxl_buffer + (row * buffer->bytes_per_row));
		for (int32 col = 0; col < buffer->width; ++col) {
			// 32-bit RGB format, [31:0] x:R:G:B 8:8:8:8 little endian
			uint8* b = (uint8*)pxl; // padding
			uint8* g = (uint8*)pxl + 1;
			uint8* r = (uint8*)pxl + 2;
			uint8* x = (uint8*)pxl + 3;
			*r = 0;
			*g = row + offset;
			*b = col + offset;
			pxl++;
		}
	}

	offset += 8;

	munmap(pxl_buffer, buffer->size);

	if (buffer->wl_buffer) {
		wl_buffer_destroy(buffer->wl_buffer);
	}

	buffer->wl_buffer = wl_shm_pool_create_buffer(buffer->wl_shm_pool, 0, buffer->width, 
			buffer->height, buffer->bytes_per_row, WL_SHM_FORMAT_XRGB8888);

	client->latest_buffer_index = render_buffer_index;

	wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);

	wl_surface_damage(client->wl_surface, 0, 0, buffer->width, buffer->height);

	wl_surface_commit(client->wl_surface);

	client->active_buffer_index = client->latest_buffer_index;

	client->wl_surface_frame = wl_surface_frame(client->wl_surface);
	wl_callback_add_listener(client->wl_surface_frame, &server->listeners.wl_surface_frame_listener,
			wayland_state);
}

int32 main(void)
{
	WaylandState wayland_state = { 0 };
	/* connection to the walyand-server */
	WaylandServerState* wayland_server = &wayland_state.server;
	WaylandListeners* wayland_listeners = &wayland_server->listeners;

	wayland_listeners->wl_registry.global = waylandRegistryEventGlobal;
	wayland_listeners->wl_registry.global_remove = waylandRegistryEventGlobalRemove;
	wayland_listeners->wl_shm.format = waylandShmEventFormat;
	wayland_listeners->xdg_wm_base.ping = waylandXdgWmBaseEventPing;
	wayland_listeners->xdg_surface.configure = waylandXdgSurfaceEventConfigure;
	wayland_listeners->xdg_toplevel.configure = waylandXdgToplevelEventConfigure;
	wayland_listeners->xdg_toplevel.close = waylandXdgToplevelEventClose;
	wayland_listeners->xdg_toplevel.configure_bounds = waylandXdgToplevelEventConfigureBounds;
	wayland_listeners->xdg_toplevel.wm_capabilities = waylandXdgToplevelEventWmCapabilities; // TODO(vluis): test limitting xdg_wm_base version to 4 and see if this throws an error
	wayland_listeners->wl_surface_frame_listener.done = waylandSurfaceEventNewFrame;

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
	wayland_client->wl_surface_frame = wl_surface_frame(wayland_client->wl_surface);
	wl_callback_add_listener(wayland_client->wl_surface_frame,
			&wayland_server->listeners.wl_surface_frame_listener, &wayland_state);

	wl_surface_commit(wayland_client->wl_surface);

	running = true;

	while (running) {
		wl_display_dispatch(wayland_server->wl_display); // process events | procesar eventos acumulados
		wl_display_flush(wayland_server->wl_display); // send requests | enviar peticiones acumuladas
	}

	wl_display_disconnect(wayland_server->wl_display);

	return EXIT_SUCCESS; // finalizar con éxito
}
