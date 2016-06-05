#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdlib.h>
#include <stdio.h>

typedef struct producer_libvlc_s *producer_libvlc;

struct producer_libvlc_s
{
	mlt_producer parent;
};

mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *file )
{
	mlt_producer producer = NULL;

	// Construct the producer
	producer_libvlc self = calloc( 1, sizeof( struct producer_libvlc_s ) );
	producer = calloc( 1, sizeof( struct mlt_producer_s ) );

	// Initialise it
	if ( mlt_producer_init( producer, self ) == 0 )
	{
		self->parent = producer;
	}

	printf( "Hello MLT!\n" );

	return producer;
}
