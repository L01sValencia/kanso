#include <stdlib.h>

#include "defines.h"
#include "types.h"
#include "render.h"

#include "log.c"
#include "linux/wayland_window.c"

void renderGradient(void* buffer, int32 width, int32 height, int32 bytes_per_row)
{

	persist int32 offset = 0;
	for (int32 row = 0; row < height; ++row) {
		uint32* pxl = (uint32*)((uint8*)buffer + (row * bytes_per_row));
		for (int32 col = 0; col < width; ++col) {
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
}

int32 main(void)
{
	WaylandState wayland_state = { 0 };
	WaylandServerState* wayland_server = &wayland_state.server;
	WaylandClientState* wayland_client = &wayland_state.client;

	waylandSetListeners(&wayland_server->listeners);
	waylandServerConnect(wayland_server);
	waylandClientInitialize(&wayland_state);

	wayland_client->running = true;
	while (wayland_client->running) {
		waylandUpdateRenderingSystem(wayland_client);
		waylandUpdate(wayland_server);
	}

	waylandServerDisconnect(wayland_server);

	return EXIT_SUCCESS; // finalizar con éxito
}
