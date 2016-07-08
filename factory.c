#include <string.h>
#include <limits.h>
#include <framework/mlt.h>

extern mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg );
extern mlt_consumer consumer_libvlc_init( mlt_profile profile, char *arg );

MLT_REPOSITORY
{
	MLT_REGISTER( producer_type, "libvlc", producer_libvlc_init );
	MLT_REGISTER( consumer_type, "libvlc", consumer_libvlc_init );
}
