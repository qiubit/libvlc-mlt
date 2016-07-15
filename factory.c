#include <string.h>
#include <limits.h>

#include <framework/mlt.h>

extern mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg );
extern mlt_consumer consumer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg );

static mlt_properties metadata( mlt_service_type type, const char *id, void *data )
{
	char file[ PATH_MAX ];
	snprintf( file, PATH_MAX, "%s/libvlc/%s", mlt_environment( "MLT_DATA" ), (char*) data );
	return mlt_properties_parse_yaml( file );
}

MLT_REPOSITORY
{
	MLT_REGISTER( producer_type, "libvlc", producer_libvlc_init );
	MLT_REGISTER( consumer_type, "libvlc", consumer_libvlc_init );

	MLT_REGISTER_METADATA( consumer_type, "libvlc", metadata, "consumer_libvlc.yml" );
}
