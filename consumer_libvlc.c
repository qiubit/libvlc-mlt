#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

// Debug code

pthread_mutex_t log_mutex;

static void log_cb( void *data, int level, const libvlc_log_t *ctx, const char *fmt, va_list args )
{
	pthread_mutex_lock( &log_mutex );
	printf( "VLC LOG: " );
	vprintf( fmt, args );
	printf( "\n" );
	pthread_mutex_unlock( &log_mutex );
}

typedef struct consumer_libvlc_s *consumer_libvlc;

struct consumer_libvlc_s
{
	mlt_consumer parent;
	libvlc_instance_t *vlc;
	libvlc_media_t *media;
	libvlc_media_player_t *media_player;
};

static void setup_vlc( consumer_libvlc self );
static int imem_get( void *data, const char* cookie, int64_t *dts, int64_t *pts,
					 unsigned *flags, size_t *bufferSize, const void **buffer );
static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer );

mlt_consumer consumer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	mlt_consumer parent = NULL;
	consumer_libvlc self = NULL;

	// Allocate the consumer data structures
	parent = mlt_consumer_new( profile );
	self = calloc( 1, sizeof( struct consumer_libvlc_s ) );
	assert( parent != NULL && self != NULL );

	mlt_properties properties = MLT_CONSUMER_PROPERTIES( parent );
	mlt_properties_set_lcnumeric( properties, "C" );
	mlt_properties_set( properties, "target", arg );
	self->parent = parent;

	self->vlc = libvlc_new( 0, NULL );

	libvlc_log_set( self->vlc, log_cb, NULL );
	pthread_mutex_init( &log_mutex, NULL );

	setup_vlc( self );
	self->media_player = libvlc_media_player_new_from_media( self->media );
	libvlc_media_player_play( self->media_player );
	return parent;
}

// Sets up input and output options in VLC
static void setup_vlc( consumer_libvlc self )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
	const char vcodec[] = "RV32";
	const char acodec[] = "s16l";

	char video_string[ 512 ];
	char audio_string[ 512 ];
	char output_string[ 512 ];
	// TODO: For some reason I need to add these options with 3 separate libvlc_media_add_option() calls.
	// Is there any way around that?
	char imem_get_conf[ 512 ];
	char imem_release_conf[ 512 ];
	char imem_data_conf[ 512 ];

	// We will create media using imem MRL
	sprintf( video_string, "imem://width=%i:height=%i:dar=%s:fps=%s/1:cookie=0:codec=%s:cat=2:caching=0",
		mlt_properties_get_int( properties, "width" ),
		mlt_properties_get_int( properties, "height" ),
		mlt_properties_get( properties, "display_ratio" ),
		mlt_properties_get( properties, "fps" ),
		vcodec );

	// Audio stream will be added as input slave
	sprintf( audio_string, ":input-slave=imem://cookie=1:cat=1:codec=s16l:samplerate=%d:channels=%d:caching=0",
		mlt_properties_get_int( properties, "frequency" ),
		mlt_properties_get_int( properties, "channels" ) );

	// This configures file output
	sprintf( output_string, ":sout=#transcode{"
		"vcodec=%s,fps=%s,width=%d,height=%d,"
		"acodec=%s,channels=%d,samplerate=%d}"
		":standard{access=file,mux=ps,dst=\"%s\"}",
		"mp2v",
		mlt_properties_get( properties, "fps" ),
		mlt_properties_get_int( properties, "width" ),
		mlt_properties_get_int( properties, "height" ),
		"mpga",
		mlt_properties_get_int( properties, "channels" ),
		mlt_properties_get_int( properties, "frequency" ),
		mlt_properties_get( properties, "target" ) );

	// This configures imem callbacks
	sprintf( imem_get_conf,
		":imem-get=%" PRIdPTR,
		(intptr_t)(void*)&imem_get );

	sprintf( imem_release_conf,
		":imem-release=%" PRIdPTR,
		(intptr_t)(void*)&imem_release );

	sprintf( imem_data_conf,
		":imem-data=%" PRIdPTR,
		(intptr_t)(void*)self );

	self->media = libvlc_media_new_location( self->vlc, video_string );
	assert( self->media != NULL );
	libvlc_media_add_option( self->media, imem_get_conf );
	libvlc_media_add_option( self->media, imem_release_conf );
	libvlc_media_add_option( self->media, imem_data_conf );
	libvlc_media_add_option( self->media, audio_string );
	libvlc_media_add_option( self->media, output_string );
}

static int imem_get( void *data, const char* cookie, int64_t *dts, int64_t *pts,
					 uint32_t *flags, size_t *bufferSize, const void **buffer )
{
	printf( "imem_getting\n" );
	*buffer = NULL;
	return 0;
}

static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer )
{
	printf( "imem_releasing\n" );
}