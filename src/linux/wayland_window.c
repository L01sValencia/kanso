/* wayland_window.c: linux platform window system */

#include "../defines.h"
#include "../types.h"
#include "../log.h"
#include "../render.h"

#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <wayland-client.h>
#include "xdg_shell_client_protocol.h"
#include "xdg_shell_protocol.c"

#define MIN_WLCOMPOSITOR_VERSION 4 // 3 if not using HiDPI support, 1 if not needing screen rotation
#define MAX_WLCOMPOSITOR_VERSION 6
#define MIN_WLSHM_VERSION 2 // TODO(vluis): Research the actual minimal version
#define MAX_WLSHM_VERSION 2
#define MIN_XDGWMBASE_VERSION 5
#define MAX_XDGWMBASE_VERSION 7

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
	int32 last_rendered_buffer_index; // ready to be shown on screen | listo para presentarse en pantalla
	int32 active_buffer_index;
	bool8 running;
} WaylandClientState;

typedef struct {
	WaylandServerState server;
	WaylandClientState client;
} WaylandState;

/*
 * [EN] Takes a null-character terminated string and replaces every occurrence of the
 * char_to_replace in the string with a letter from 'A' to 'P' or 'a' to 'p'.
 * [ES] Toma un string finalizado con el caracter nulo y reemplaza cada aparición del caracter
 * char_to_replace por una letra desde 'A' hasta 'P', o bien, desde 'a' hasta 'p'.
 */
[[nodiscard]] internal bool8 linuxRandomizeCharacterInString(char string[], char char_to_replace)
{
	struct timespec time;
	uint64 random = 0;
	for (char* c = string; *c != '\0'; ++c) {
		if (random <= 0b0010'0000) {
			if (clock_gettime(CLOCK_REALTIME, &time) < 0) {
				logWarn("Linux platform: couldn't get time from clock_gettime().");
				return false;
			} else {
				random = time.tv_nsec * time.tv_nsec;
			}
		}
		if (*c == char_to_replace) {
			*c = 'A' + (0b0010'0000 & random) + (0b0000'1111 & random); // A-P o a-p
			random >>= 5;
		}
	}
	return true;
}


// TODO(vluis): Move to platform_linux.c
[[nodiscard]] bool8 linuxCreateShmObject(int64 size, int32* fd)
{
	int32 retries = 16; // number of times the shm_open operation could fail before it stops trying
	char shm_name[] = "/kanso_shm_$$$$";
	while (retries > 0) {
		if (!linuxRandomizeCharacterInString(shm_name, '$')) {
			retries--;
			continue;
		}
		*fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (*fd >= 0) { // valid
			break;
		}
		char* name = shm_name;
		name = "/kanso_shm_$$$$";
		retries--;
	};

	if (*fd < 0) {
		// TODO(vluis): is errno available? log it?
		return false;
	}
	shm_unlink(shm_name);
	ftruncate(*fd, size);

	return true;
}

[[nodiscard]] internal bool8 waylandSetUpBuffer(WaylandBuffer* buffer, int32 new_width,
		int32 new_height, struct wl_shm* wl_shm)
{
	/* cleanup | limpieza */
	if (buffer->wl_buffer) {
		wl_buffer_destroy(buffer->wl_buffer);
		buffer->wl_buffer = nullptr;
	}
	if (buffer->wl_shm_pool) {
		wl_shm_pool_destroy(buffer->wl_shm_pool);
		buffer->wl_shm_pool = nullptr;
	}
	if (buffer->fd >= 0) {
		close(buffer->fd);
		buffer->fd = -1;
	}

	/* construction | construcción */
	if (new_width == 0 || new_height == 0) {
		buffer->width = STD_WIDTH;
		buffer->height = STD_HEIGHT;
	} else {
		buffer->width = new_width;
		buffer->height = new_height;
	}
	buffer->bytes_per_row = buffer->width * BYTES_PER_PXL; // stride (in bytes)
	buffer->size = buffer->bytes_per_row * buffer->height; // pixel buffer size (in bytes)
	if (!linuxCreateShmObject(buffer->size, &buffer->fd)) {
		logFatal("Failed to open a POSIX shared memory object for pixel buffer allocation.");
		return false;
	}
	buffer->wl_shm_pool = wl_shm_create_pool(wl_shm, buffer->fd, buffer->size);
	buffer->wl_buffer = wl_shm_pool_create_buffer(buffer->wl_shm_pool, 0, buffer->width,
			buffer->height, buffer->bytes_per_row, WL_SHM_FORMAT_XRGB8888);

	return true;
}

/*
 * [EN] Guarantees the successful binding to the requested global object, otherwise it will abort()
 * the program.
 * [ES] Garantiza el vínculo exitoso al objeto global solicitado, de otra forma abortará (abort())
 * el programa.
 */
[[nodiscard]] internal void* waylandBindToGlobalObject(struct wl_display* display,
		struct wl_registry* registry, const struct wl_interface* object_interface,
		uint32 object_name, uint32 server_supported_version, uint32 client_min_supported_version,
		uint32 client_max_supported_version)
{
	uint32 object_version = server_supported_version;
	if (server_supported_version < client_min_supported_version) {
		logFatal("The version (%u) of the wayland global object %s, is lower than the minimal "
				"supported by kanso (%u).", server_supported_version, object_interface->name,
				client_min_supported_version);
		goto error;
	} else if (server_supported_version > client_max_supported_version) {
		object_version = client_max_supported_version;
	}
	void* object = wl_registry_bind(registry, object_name, object_interface, object_version);
	return object;

	error:
		wl_display_disconnect(display);
		abort();
}

/*
 * [EN] The wl_registry global object notifies the availability of global objects.
 * [ES] El objeto global wl_registry notifica la disponibilidad de objetos globales.
 */
internal void waylandRegistryEventGlobal(void *data, struct wl_registry *registry,
		uint32 object_name, const char *interface_name, uint32 interface_version)
{
	WaylandServerState* server = data;

	// TODO(vluis): explore if (!0) is faster than (0 == 0) in the context of if statements
	if (!strcmp(wl_compositor_interface.name, interface_name)) {
		server->wl_compositor = waylandBindToGlobalObject(server->wl_display, server->wl_registry,
				&wl_compositor_interface, object_name, interface_version, MIN_WLCOMPOSITOR_VERSION,
				MAX_WLCOMPOSITOR_VERSION);
		logInfo("Successful bind to the wayland global object %s", interface_name);
	}
	else if (!strcmp(wl_shm_interface.name, interface_name)) {
		server->wl_shm = waylandBindToGlobalObject(server->wl_display, server->wl_registry,
				&wl_shm_interface, object_name, interface_version, MIN_WLSHM_VERSION,
				MAX_WLSHM_VERSION);
		wl_shm_add_listener(server->wl_shm, &server->listeners.wl_shm, nullptr);
		logInfo("Successful bind to the wayland global object %s", interface_name);
	}
	else if (!strcmp(xdg_wm_base_interface.name, interface_name)) {
		server->xdg_wm_base = waylandBindToGlobalObject(server->wl_display, server->wl_registry,
				&xdg_wm_base_interface, object_name, interface_version, MIN_XDGWMBASE_VERSION,
				MAX_XDGWMBASE_VERSION);
		xdg_wm_base_add_listener(server->xdg_wm_base, &server->listeners.xdg_wm_base, nullptr);
		logInfo("Successful bind to the wayland global object %s", interface_name);
	}
}

/*
 * [EN] The wl_registry global object notifies the removal of previously available global objects.
 * [ES] El objeto global wl_registry notifica la eliminación de objetos globales previamente
 * disponibles.
 */
internal void waylandRegistryEventGlobalRemove(void *data, struct wl_registry *registry,
		uint32 object_name)
{
	/* Intentionally left blank | Intencionalmente en blanco */
}

/*
 * [EN] The wl_shm global object informs about a valid pixel format that can be used for pixel
 * buffers.
 * [ES] El objeto global wl_shm informa sobre un formato válido de píxeles que puede ser utilizado
 * por 'buffers' de píxeles.
 */
internal void waylandShmEventFormat(void *data, struct wl_shm* shm, uint32 pxl_format)
{
	/* 
	 * [EN] Intentionally left blank since we're using one of the two 'always supported' formats:
	 * argb8888 and xrgb8888.
	 * [ES] Intencionalmente en blanco ya que vamos a utilizar uno de los dos formatos siempre
	 * disponibles: argb8888 y xrgb8888.
	 */
}

/*
 * [EN] The xdg_wm_base global object asks the application if it is still responsive (running ok).
 * [ES] El objeto global xdg_wm_base pregunta a la aplicación si ésta sigue responsiva (corriendo).
 */
internal void waylandXdgWmBaseEventPing(void *data, struct xdg_wm_base *xdg_wm_base, uint32 serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

/*
 * [EN] The xdg_surface global object issues the final configuration event for a surface.
 * [ES] El objeto global xdg_surface expide el evento final de configuración para una superficie.
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
	wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_commit(client->wl_surface);
	first_call_done = true;
}

/*
 * [EN] The xdg_toplevel global_object issues a configuration event for a surface, suggesting a
 * change in the surface size.
 * [ES] El objeto global xdg_toplevel expide un evento de configuración para una superficie,
 * sugiriendo un cambio en tamaño de la superficie.
 */
internal void waylandXdgToplevelEventConfigure(void *data, struct xdg_toplevel *xdg_toplevel,
		int32 suggested_new_width, int32 suggested_new_height, struct wl_array *surface_states)
{
	/* NOTE(vluis): maybe consider the surface_states?
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

	for (int32 i = 0; i < NUMBER_OF_BUFFERS; ++i) {
		bool8 succeded = waylandSetUpBuffer(&client->buffers[i], suggested_new_width,
				suggested_new_height, server->wl_shm);
		if (!succeded) {
			logFatal("Failed to set up buffers for wayland.");
			abort();
		}
	}
	client->active_buffer_index = -1; // no buffer is active (attached to a surface)

	/*
	 * [EN] NOTE(vluis): In a real-time application (like this one) we can avoid repaint and assign
	 * a new buffer to the wl_surface here, because we're forcing full-size repaints (frames)
	 * continuously through wl_surface_frame.
	 * [ES] En una aplicación en tiempo real (como ésta) podemos evitar repintar y asignar un nuevo
	 * buffer a la superficie 'wl_surface' aquí, ya que estamos forzando repintados de tamaño
	 * completo (nuevos fotogramas) continuamente a través de wl_surface_frame.
	 */
}

/*
 * [EN] The xdg_toplevel global object informs that the user is requesting for the surface to close.
 * [ES] El objeto global xdg_toplevel informa que el usuario está pidiendo que la superficie sea
 * cerrada.
 */
internal void waylandXdgToplevelEventClose(void *data, struct xdg_toplevel *xdg_toplevel)
{
	// TODO(vluis): Send dialog to user to confirm exit, before actually closing the surface
	WaylandState* wayland_state = data;
	wayland_state->client.running = false;
}

/*
 * [EN] The xdg_toplevel global object communicates the size that a surface is recommended to
 * constrain to.
 * [ES] El objeto global xdg_toplevel comunica el tamaño al cual es recomendado limitar una
 * superficie.
 */
internal void waylandXdgToplevelEventConfigureBounds(void *data, struct xdg_toplevel *xdg_toplevel,
		int32 suggested_max_width, int32 suggested_max_height)
{
	// Intentionally left blank, for now

	/*
	 * TODO(vluis): if width == 0 || height == 0 { ignore the event } // bounds unknown
	 * otherwise { inform the rendering system of the max_width and max_height suggestions for
	 * setting up the rendering resolution }.
	 */
}

/*
 * [EN] The xdg_toplevel global object advertises the capabilities supported by the compositor
 * regarding to the presentation of surfaces.
 * [ES] El objeto global xdg_toplevel anuncia las capacidades soportadas por el compositor
 * referentes a la presentación de superficies.
 */
void waylandXdgToplevelEventWmCapabilities(void *data, struct xdg_toplevel *xdg_toplevel,
		struct wl_array *compositor_capabilities)
{
	// Intentionally left blank, for now
	// TODO(vluis): Consider these befor allowing operations like: fullscreen / minimize / ...?
	/* capabilities:
	 *    1 window_menu 
	 *    2 maximize
	 *    3 fullscreen
	 *    4 minimize
	 */
}

/*
 * [EN] The wl_surface's wl_callback global object notifies that the client should start drawing a 
 * new frame.
 * [ES] El objeto global wl_callback de wl_surface notifica que el cliente debería empezar a dibujar
 * un nuevo fotograma.
 */
void waylandSurfaceEventNewFrame(void *data, struct wl_callback *callback, uint32 current_time)
{	
	// TODO(vluis): use the previous and the current_time to estimate and log a framerate
	/* initial render */

	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

	wl_callback_destroy(callback);
	client->wl_surface_frame = wl_surface_frame(client->wl_surface);
	wl_callback_add_listener(client->wl_surface_frame, &server->listeners.wl_surface_frame_listener,
			wayland_state);

	WaylandBuffer* buffer = &client->buffers[client->last_rendered_buffer_index];

	wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(client->wl_surface, 0, 0, buffer->width, buffer->height);
	wl_surface_commit(client->wl_surface);
	client->active_buffer_index = client->last_rendered_buffer_index;
}

/*
 * [EN] Sets wayland events callback functions.
 * [ES] Configura las funciones callback de los eventos wayland.
 */
internal void waylandSetListeners(WaylandListeners* listeners)
{
	listeners->wl_registry.global = waylandRegistryEventGlobal;
	listeners->wl_registry.global_remove = waylandRegistryEventGlobalRemove;
	listeners->wl_shm.format = waylandShmEventFormat;
	listeners->xdg_wm_base.ping = waylandXdgWmBaseEventPing;
	listeners->xdg_surface.configure = waylandXdgSurfaceEventConfigure;
	listeners->xdg_toplevel.configure = waylandXdgToplevelEventConfigure;
	listeners->xdg_toplevel.close = waylandXdgToplevelEventClose;
	listeners->xdg_toplevel.configure_bounds = waylandXdgToplevelEventConfigureBounds;
	listeners->xdg_toplevel.wm_capabilities = waylandXdgToplevelEventWmCapabilities; // TODO(vluis): test limiting xdg_wm_base version to 4 and see if this throws an error
	listeners->wl_surface_frame_listener.done = waylandSurfaceEventNewFrame;
}

/*
 * [EN] Establishes a connection to the wayland server and begins the process of getting the needed
 * global objects.
 * [ES] Establece una conexión al servidor wayland y comienza el proceso de obtener los objetos
 * globales necesarios.
 */
internal void waylandServerConnect(WaylandServerState* server)
{
	server->wl_display = wl_display_connect(nullptr);
	server->wl_registry = wl_display_get_registry(server->wl_display);
	wl_registry_add_listener(server->wl_registry, &server->listeners.wl_registry,
			server);
	wl_display_roundtrip(server->wl_display); // wait for wl_registry events to process
}

internal void waylandServerDisconnect(WaylandServerState* server)
{
	wl_display_disconnect(server->wl_display);
}

internal void waylandClientInitialize(WaylandState* state)
{
	WaylandServerState* server = &state->server;
	WaylandClientState* client = &state->client;

	for (int32 i = 0; i < NUMBER_OF_BUFFERS; ++i) { // initialization
		client->buffers[i].fd = -1;
	}

	client->wl_surface = wl_compositor_create_surface(server->wl_compositor);
	client->xdg_surface = xdg_wm_base_get_xdg_surface(server->xdg_wm_base, client->wl_surface);
	xdg_surface_add_listener(client->xdg_surface, &server->listeners.xdg_surface, state);
	client->xdg_toplevel = xdg_surface_get_toplevel(client->xdg_surface);
	xdg_toplevel_add_listener(client->xdg_toplevel, &server->listeners.xdg_toplevel, state);
	xdg_toplevel_set_title(client->xdg_toplevel, "kanso");
	client->wl_surface_frame = wl_surface_frame(client->wl_surface);
	wl_callback_add_listener(client->wl_surface_frame, &server->listeners.wl_surface_frame_listener,
			state);
	wl_surface_commit(client->wl_surface);
}

[[nodiscard]] internal int32 waylandSelectBufferForNewFrame(WaylandClientState* client)
{
	int32 next_buffer_index; // buffer for the new frame
	int32 active_buffer_index = client->active_buffer_index;
	assert(NUMBER_OF_BUFFERS >= 2, "Invalid number of buffers being used");
	if (active_buffer_index < 0) { // buffers are not being used
		next_buffer_index = 0;
	} else {
		for (int32 i = 1; i < NUMBER_OF_BUFFERS; ++i) {
			next_buffer_index = (active_buffer_index + i) % NUMBER_OF_BUFFERS;
			if (next_buffer_index != client->last_rendered_buffer_index)
				break;
		}
	}
	// NOTE(vluis): In case of using only two buffers we have to prevent wayland from reading from
	// this buffer until the render is complete. Otherwise wayland could present an incomplete
	// rendered frame. This could happen when our rendering speed is faster than presentation speed,
	// in which case the new_frame_buffer is being rendered to multiple times.
	return next_buffer_index;
}

internal void waylandUpdateRenderingSystem(WaylandClientState* client)
{
	int32 next_buffer_index = waylandSelectBufferForNewFrame(client);
	WaylandBuffer* next_buffer = &client->buffers[next_buffer_index];
	void* next_buffer_mem = mmap(nullptr, next_buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			next_buffer->fd, 0);
	renderGradient(next_buffer_mem, next_buffer->width, next_buffer->height,
			next_buffer->bytes_per_row);
	munmap(next_buffer_mem, next_buffer->size);
	client->last_rendered_buffer_index = next_buffer_index;
}

/*
 * [EN] Dispatches queued wayland messages.
 * [ES] Despacha los mensajes wayland acumulados.
 */
internal void waylandUpdate(WaylandServerState* server)
{
		wl_display_dispatch(server->wl_display); // process queued events
		wl_display_flush(server->wl_display); // send queued requests
}

/* 11/12/2025 Luis Arturo Ramos Valencia - kanso engine */
