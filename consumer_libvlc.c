#include <framework/mlt.h>

#include <stdio.h>

mlt_consumer consumer_libvlc_init( mlt_profile profile, char *arg )
{
	printf( "Hello MLT\n" );

	// Allocate the consumer
	mlt_consumer consumer = mlt_consumer_new( profile );

	return consumer;
}