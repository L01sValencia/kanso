/* wayland_window.c: linux platform window system */

#include "../defines.h"
#include "../types.h"
#include "../log.h"
#include "../render.h"

// needed for wayland client's presentation
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

// needed for wayland client's input processing
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include "xdg_shell_client_protocol.h"
#include "xdg_shell_protocol.c"

#define MIN_WLCOMPOSITOR_VERSION 4 // 3 if not using HiDPI support, 1 if not needing screen rotation
#define MAX_WLCOMPOSITOR_VERSION 6
#define MIN_WLSHM_VERSION 2 // TODO(vluis): Research the actual minimal version
#define MAX_WLSHM_VERSION 2
#define MIN_SEAT_VERSION 7 // TODO(vluis): Research the actual minimal version
#define MAX_SEAT_VERSION 10
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
	struct wl_seat_listener wl_seat;
	struct wl_pointer_listener wl_pointer;
} WaylandListeners;

typedef struct {
	struct wl_display* wl_display;
	struct wl_registry* wl_registry;
	struct wl_compositor* wl_compositor;
	struct wl_seat* wl_seat;
	struct xdg_wm_base* xdg_wm_base;
	struct wl_shm* wl_shm;
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
	struct wl_keyboard* wl_keyboard;
	struct wl_pointer* wl_pointer;
	WaylandBuffer buffers[NUMBER_OF_BUFFERS];
	int32 last_rendered_buffer_index; // ready to be shown on screen | listo para presentarse en pantalla
	int32 active_buffer_index;
	uint32 animation_speed;
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
		wl_display_disconnect(display);
		abort();
	} else if (server_supported_version > client_max_supported_version) {
		object_version = client_max_supported_version;
	}
	void* object = wl_registry_bind(registry, object_name, object_interface, object_version);
	return object;
}

/*
 * [EN] The wl_registry global object notifies the availability of global objects.
 * [ES] El objeto global wl_registry notifica la disponibilidad de objetos globales.
 */
internal void waylandRegistryEventGlobal(void* data, struct wl_registry* registry,
		uint32 object_name, const char *interface_name, uint32 interface_version)
{
	WaylandState* wayland_state = data;
	WaylandServerState* server = &wayland_state->server;
	WaylandClientState* client = &wayland_state->client;

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
	else if (!strcmp(wl_seat_interface.name, interface_name)) {
		server->wl_seat = waylandBindToGlobalObject(server->wl_display, server->wl_registry,
				&wl_seat_interface, object_name, interface_version, MIN_SEAT_VERSION,
				MAX_SEAT_VERSION);
		wl_seat_add_listener(server->wl_seat, &server->listeners.wl_seat, wayland_state);
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
internal void waylandRegistryEventGlobalRemove(void* data, struct wl_registry* registry,
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
internal void waylandShmEventFormat(void* data, struct wl_shm* shm, uint32 pxl_format)
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
internal void waylandXdgWmBaseEventPing(void *data, struct xdg_wm_base* xdg_wm_base, uint32 serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

/*
 * [EN] The xdg_surface global object issues the final configuration event for a surface.
 * [ES] El objeto global xdg_surface expide el evento final de configuración para una superficie.
 */
internal void waylandXdgSurfaceEventConfigure(void* data, struct xdg_surface* xdg_surface,
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
internal void waylandXdgToplevelEventConfigure(void* data, struct xdg_toplevel* xdg_toplevel,
		int32 suggested_new_width, int32 suggested_new_height, struct wl_array* surface_states)
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
internal void waylandXdgToplevelEventClose(void* data, struct xdg_toplevel* xdg_toplevel)
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
internal void waylandXdgToplevelEventConfigureBounds(void* data, struct xdg_toplevel* xdg_toplevel,
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
void waylandXdgToplevelEventWmCapabilities(void* data, struct xdg_toplevel* xdg_toplevel,
		struct wl_array* compositor_capabilities)
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
void waylandSurfaceEventNewFrame(void* data, struct wl_callback* callback, uint32 current_time)
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
 * [EN] The wl_seat global object announces changes in input capabilities.
 * [ES] El objeto global wl_seat anuncia cambios en capacidades de entrada.
 */
void waylandSeatEventCapabilities(void* data, struct wl_seat* seat, uint32 capabilities)
{
	WaylandState* wayland_state = data;
	WaylandClientState* client = &wayland_state->client;
	WaylandServerState* server = &wayland_state->server;

	if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		assert(!client->wl_keyboard, "Didn't released wl_keyboard when the capability was lost.");
		client->wl_keyboard = wl_seat_get_keyboard(seat);
	} else if (client->wl_keyboard) {
		wl_keyboard_destroy(client->wl_keyboard);
		client->wl_keyboard = nullptr;
	}

	if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
		assert(!client->wl_pointer, "Didn't released wl_pointer when the capability was lost.");
		client->wl_pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(client->wl_pointer, &server->listeners.wl_pointer, client);
	} else if (client->wl_pointer) {
		wl_pointer_destroy(client->wl_pointer);
		client->wl_pointer = nullptr;
	}
}

void waylandSeatEventName(void* data, struct wl_seat* seat, const char* name)
{
	// Intentionally left blank
	// TODO(vluis): Is this needed for my simple client?
}

/**
 * enter event
 *
 * Notification that this seat's pointer is focused on a certain
 * surface.
 *
 * When a seat's focus enters a surface, the pointer image is
 * undefined and a client should respond to this event by setting
 * an appropriate pointer image with the set_cursor request.
 * @param serial serial number of the enter event
 * @param surface surface entered by the pointer
 * @param surface_x surface-local x coordinate
 * @param surface_y surface-local y coordinate
 */
void waylandPointerEventEnter(void* data, struct wl_pointer* pointer, uint32 serial,
		struct wl_surface* surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	WaylandClientState* client = data;
	// client->animation_speed = 5;
}

/**
 * leave event
 *
 * Notification that this seat's pointer is no longer focused on
 * a certain surface.
 *
 * The leave notification is sent before the enter notification for
 * the new focus.
 * @param serial serial number of the leave event
 * @param surface surface left by the pointer
 */
void waylandPointerEventLeave(void* data, struct wl_pointer* pointer, uint32 serial,
		struct wl_surface* surface)
{
	WaylandClientState* client = data;
	// client->animation_speed = 0;
}

/**
 * pointer motion event
 *
 * Notification of pointer location change. The arguments
 * surface_x and surface_y are the location relative to the focused
 * surface.
 * @param time timestamp with millisecond granularity
 * @param surface_x surface-local x coordinate
 * @param surface_y surface-local y coordinate
 */
void waylandPointerEventMotion(void* data, struct wl_pointer* pointer, uint32 time,
		wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	float64 mouse_x = wl_fixed_to_double(surface_x);
	float64 mouse_y = wl_fixed_to_double(surface_y);
	logTrace("mouse position = (%lf, %lf)", mouse_x, mouse_y);
}

/**
 * pointer button event
 *
 * Mouse button click and release notifications.
 *
 * The location of the click is given by the last motion or enter
 * event. The time argument is a timestamp with millisecond
 * granularity, with an undefined base.
 *
 * The button is a button code as defined in the Linux kernel's
 * linux/input-event-codes.h header file, e.g. BTN_LEFT.
 *
 * Any 16-bit button code value is reserved for future additions to
 * the kernel's event code list. All other button codes above
 * 0xFFFF are currently undefined but may be used in future
 * versions of this protocol.
 * @param serial serial number of the button event
 * @param time timestamp with millisecond granularity
 * @param button button that produced the event
 * @param state physical state of the button
 */
void waylandPointerEventButton(void* data, struct wl_pointer* pointer, uint32 serial, uint32 time,
		uint32 button, uint32 state)
{
	WaylandClientState* client = data;

	#define BTN_PRESSED 1
	#define BTN_RELEASED 0

	if (button == BTN_LEFT && state == BTN_PRESSED) {
		client->animation_speed = -5;
	} else if (button == BTN_LEFT && state == BTN_RELEASED) {
		client->animation_speed = 0;
	} else if (button == BTN_RIGHT && state == BTN_PRESSED) { 
		client->animation_speed = 5;
	} else if (button == BTN_RIGHT && state == BTN_RELEASED) {
		client->animation_speed = 0;
	}
}

/**
 * axis event
 *
 * Scroll and other axis notifications.
 *
 * For scroll events (vertical and horizontal scroll axes), the
 * value parameter is the length of a vector along the specified
 * axis in a coordinate space identical to those of motion events,
 * representing a relative movement along the specified axis.
 *
 * For devices that support movements non-parallel to axes multiple
 * axis events will be emitted.
 *
 * When applicable, for example for touch pads, the server can
 * choose to emit scroll events where the motion vector is
 * equivalent to a motion event vector.
 *
 * When applicable, a client can transform its content relative to
 * the scroll distance.
 * @param time timestamp with millisecond granularity
 * @param axis axis type
 * @param value length of vector in surface-local coordinate space
 */
void waylandPointerEventAxis(void* data, struct wl_pointer* pointer, uint32 time, uint32 axis,
		wl_fixed_t value)
{
}

/**
 * end of a pointer event sequence
 *
 * Indicates the end of a set of events that logically belong
 * together. A client is expected to accumulate the data in all
 * events within the frame before proceeding.
 *
 * All wl_pointer events before a wl_pointer.frame event belong
 * logically together. For example, in a diagonal scroll motion the
 * compositor will send an optional wl_pointer.axis_source event,
 * two wl_pointer.axis events (horizontal and vertical) and finally
 * a wl_pointer.frame event. The client may use this information to
 * calculate a diagonal vector for scrolling.
 *
 * When multiple wl_pointer.axis events occur within the same
 * frame, the motion vector is the combined motion of all events.
 * When a wl_pointer.axis and a wl_pointer.axis_stop event occur
 * within the same frame, this indicates that axis movement in one
 * axis has stopped but continues in the other axis. When multiple
 * wl_pointer.axis_stop events occur within the same frame, this
 * indicates that these axes stopped in the same instance.
 *
 * A wl_pointer.frame event is sent for every logical event group,
 * even if the group only contains a single wl_pointer event.
 * Specifically, a client may get a sequence: motion, frame,
 * button, frame, axis, frame, axis_stop, frame.
 *
 * The wl_pointer.enter and wl_pointer.leave events are logical
 * events generated by the compositor and not the hardware. These
 * events are also grouped by a wl_pointer.frame. When a pointer
 * moves from one surface to another, a compositor should group the
 * wl_pointer.leave event within the same wl_pointer.frame.
 * However, a client must not rely on wl_pointer.leave and
 * wl_pointer.enter being in the same wl_pointer.frame.
 * Compositor-specific policies may require the wl_pointer.leave
 * and wl_pointer.enter event being split across multiple
 * wl_pointer.frame groups.
 * @since 5
 */
void waylandPointerEventFrame(void* data, struct wl_pointer* pointer)
{
}

/**
 * axis source event
 *
 * Source information for scroll and other axes.
 *
 * This event does not occur on its own. It is sent before a
 * wl_pointer.frame event and carries the source information for
 * all events within that frame.
 *
 * The source specifies how this event was generated. If the source
 * is wl_pointer.axis_source.finger, a wl_pointer.axis_stop event
 * will be sent when the user lifts the finger off the device.
 *
 * If the source is wl_pointer.axis_source.wheel,
 * wl_pointer.axis_source.wheel_tilt or
 * wl_pointer.axis_source.continuous, a wl_pointer.axis_stop event
 * may or may not be sent. Whether a compositor sends an axis_stop
 * event for these sources is hardware-specific and
 * implementation-dependent; clients must not rely on receiving an
 * axis_stop event for these scroll sources and should treat scroll
 * sequences from these scroll sources as unterminated by default.
 *
 * This event is optional. If the source is unknown for a
 * particular axis event sequence, no event is sent. Only one
 * wl_pointer.axis_source event is permitted per frame.
 *
 * The order of wl_pointer.axis_discrete and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis_source source of the axis event
 * @since 5
 */
void waylandPointerEventAxisSource(void* data, struct wl_pointer* pointer, uint32 axis_source)
{
}

/**
 * axis stop event
 *
 * Stop notification for scroll and other axes.
 *
 * For some wl_pointer.axis_source types, a wl_pointer.axis_stop
 * event is sent to notify a client that the axis sequence has
 * terminated. This enables the client to implement kinetic
 * scrolling. See the wl_pointer.axis_source documentation for
 * information on when this event may be generated.
 *
 * Any wl_pointer.axis events with the same axis_source after this
 * event should be considered as the start of a new axis motion.
 *
 * The timestamp is to be interpreted identical to the timestamp in
 * the wl_pointer.axis event. The timestamp value may be the same
 * as a preceding wl_pointer.axis event.
 * @param time timestamp with millisecond granularity
 * @param axis the axis stopped with this event
 * @since 5
 */
void waylandPointerEventAxisStop(void* data, struct wl_pointer* pointer, uint32 time, uint32 axis)
{
}

/**
 * axis click event
 *
 * Discrete step information for scroll and other axes.
 *
 * This event carries the axis value of the wl_pointer.axis event
 * in discrete steps (e.g. mouse wheel clicks).
 *
 * This event is deprecated with wl_pointer version 8 - this event
 * is not sent to clients supporting version 8 or later.
 *
 * This event does not occur on its own, it is coupled with a
 * wl_pointer.axis event that represents this axis value on a
 * continuous scale. The protocol guarantees that each
 * axis_discrete event is always followed by exactly one axis event
 * with the same axis number within the same wl_pointer.frame. Note
 * that the protocol allows for other events to occur between the
 * axis_discrete and its coupled axis event, including other
 * axis_discrete or axis events. A wl_pointer.frame must not
 * contain more than one axis_discrete event per axis type.
 *
 * This event is optional; continuous scrolling devices like
 * two-finger scrolling on touchpads do not have discrete steps and
 * do not generate this event.
 *
 * The discrete value carries the directional information. e.g. a
 * value of -2 is two steps towards the negative direction of this
 * axis.
 *
 * The axis number is identical to the axis number in the
 * associated axis event.
 *
 * The order of wl_pointer.axis_discrete and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis axis type
 * @param discrete number of steps
 * @since 5
 * @deprecated Deprecated since version 8
 */
void waylandPointerEventAxisDiscrete(void* data, struct wl_pointer* pointer, uint32 axis,
		int32 discrete)
{
}

/**
 * axis high-resolution scroll event
 *
 * Discrete high-resolution scroll information.
 *
 * This event carries high-resolution wheel scroll information,
 * with each multiple of 120 representing one logical scroll step
 * (a wheel detent). For example, an axis_value120 of 30 is one
 * quarter of a logical scroll step in the positive direction, a
 * value120 of -240 are two logical scroll steps in the negative
 * direction within the same hardware event. Clients that rely on
 * discrete scrolling should accumulate the value120 to multiples
 * of 120 before processing the event.
 *
 * The value120 must not be zero.
 *
 * This event replaces the wl_pointer.axis_discrete event in
 * clients supporting wl_pointer version 8 or later.
 *
 * Where a wl_pointer.axis_source event occurs in the same
 * wl_pointer.frame, the axis source applies to this event.
 *
 * The order of wl_pointer.axis_value120 and wl_pointer.axis_source
 * is not guaranteed.
 * @param axis axis type
 * @param value120 scroll distance as fraction of 120
 * @since 8
 */
void waylandPointerEventAxisValue120(void* data, struct wl_pointer* pointer, uint32 axis,
		int32 value120)
{
}

/**
 * axis relative physical direction event
 *
 * Relative directional information of the entity causing the
 * axis motion.
 *
 * For a wl_pointer.axis event, the
 * wl_pointer.axis_relative_direction event specifies the movement
 * direction of the entity causing the wl_pointer.axis event. For
 * example: - if a user's fingers on a touchpad move down and this
 * causes a wl_pointer.axis vertical_scroll down event, the
 * physical direction is 'identical' - if a user's fingers on a
 * touchpad move down and this causes a wl_pointer.axis
 * vertical_scroll up scroll up event ('natural scrolling'), the
 * physical direction is 'inverted'.
 *
 * A client may use this information to adjust scroll motion of
 * components. Specifically, enabling natural scrolling causes the
 * content to change direction compared to traditional scrolling.
 * Some widgets like volume control sliders should usually match
 * the physical direction regardless of whether natural scrolling
 * is active. This event enables clients to match the scroll
 * direction of a widget to the physical direction.
 *
 * This event does not occur on its own, it is coupled with a
 * wl_pointer.axis event that represents this axis value. The
 * protocol guarantees that each axis_relative_direction event is
 * always followed by exactly one axis event with the same axis
 * number within the same wl_pointer.frame. Note that the protocol
 * allows for other events to occur between the
 * axis_relative_direction and its coupled axis event.
 *
 * The axis number is identical to the axis number in the
 * associated axis event.
 *
 * The order of wl_pointer.axis_relative_direction,
 * wl_pointer.axis_discrete and wl_pointer.axis_source is not
 * guaranteed.
 * @param axis axis type
 * @param direction physical direction relative to axis motion
 * @since 9
 */
void waylandPointerEventAxisRelativeDirection(void* data, struct wl_pointer* pointer, uint32 axis,
		uint32 direction)
{
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
	listeners->wl_seat.capabilities = waylandSeatEventCapabilities;
	listeners->wl_seat.name = waylandSeatEventName;
	listeners->wl_pointer.enter = waylandPointerEventEnter;
	listeners->wl_pointer.leave = waylandPointerEventLeave;
	listeners->wl_pointer.motion = waylandPointerEventMotion;
	listeners->wl_pointer.button = waylandPointerEventButton;
	listeners->wl_pointer.axis = waylandPointerEventAxis;
	listeners->wl_pointer.frame = waylandPointerEventFrame;
	listeners->wl_pointer.axis_source = waylandPointerEventAxisSource;
	listeners->wl_pointer.axis_stop = waylandPointerEventAxisStop;
	listeners->wl_pointer.axis_discrete = waylandPointerEventAxisDiscrete;
	listeners->wl_pointer.axis_value120 = waylandPointerEventAxisValue120;
	listeners->wl_pointer.axis_relative_direction = waylandPointerEventAxisRelativeDirection;
}

/*
 * [EN] Establishes a connection to the wayland server and begins the process of getting the needed
 * global objects.
 * [ES] Establece una conexión al servidor wayland y comienza el proceso de obtener los objetos
 * globales necesarios.
 */
internal void waylandServerConnect(WaylandState* wayland_state)
{
	WaylandServerState* server = &wayland_state->server;
	server->wl_display = wl_display_connect(nullptr);
	server->wl_registry = wl_display_get_registry(server->wl_display);
	wl_registry_add_listener(server->wl_registry, &server->listeners.wl_registry, wayland_state);
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
			next_buffer->bytes_per_row, client->animation_speed);
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
