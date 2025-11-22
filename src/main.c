#include "define.h"
#include "types.h"
#include <stdlib.h>
#include <stdio.h>

global int64 a;

local uint64 incrementCounter(void)
{
	persist uint64 c = 0;
	c++;

	return c;
}

int32 main(void)
{
	for (int i = 0; i < 10; ++i) {
		printf("%lu\n", a = incrementCounter());
	}
	printf("a = %lu\n", a);

	return EXIT_SUCCESS; // Finalizar con éxito
}

