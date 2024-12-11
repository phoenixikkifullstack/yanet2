#include <stdio.h>

#include "dataplane.h"


int
main(int argc, char **argv)
{
	struct dataplane dataplane;

	// FIXME: dataplane configuration
	// FIXME: dataplane error handling
	dataplane_init(
		&dataplane,
		argv[0],
		argc - 1,
		(const char **)argv + 1);

	dataplane_start(&dataplane);

	// FIXME: infinite sleep effictively
	dataplane_stop(&dataplane);

	return 0;
}
