#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define VIDEO_COOKIE 0
#define AUDIO_COOKIE 1

#define INITIAL_POSITION -1

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
	libvlc_event_manager_t *mp_manager;
	int64_t latest_video_pts;
	int64_t latest_audio_pts;
	mlt_deque frame_queue;
	pthread_mutex_t queue_mutex;
	mlt_position video_position;
	mlt_position audio_position;
	void *video_imem_data;
	void *audio_imem_data;
	int running;
};

static void setup_vlc( consumer_libvlc self );
static int imem_get( void *data, const char* cookie, int64_t *dts, int64_t *pts,
					 unsigned *flags, size_t *bufferSize, void **buffer );
static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer );
static int consumer_start( mlt_consumer parent );
static int consumer_stop( mlt_consumer parent );
static int consumer_is_stopped( mlt_consumer parent );
static void consumer_close( mlt_consumer parent );
static void consumer_purge( mlt_consumer parent );
static void mp_callback( const struct libvlc_event_t *evt, void *data );

mlt_consumer consumer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	int err;
	mlt_consumer parent = NULL;
	consumer_libvlc self = NULL;

	// Allocate the consumer data structures
	parent = calloc( 1, sizeof( struct mlt_consumer_s ) );
	self = calloc( 1, sizeof( struct consumer_libvlc_s ) );
	assert( parent != NULL && self != NULL );
	err = mlt_consumer_init( parent, self, profile );
	assert( err == 0 );

	mlt_properties properties = MLT_CONSUMER_PROPERTIES( parent );
	mlt_properties_set_lcnumeric( properties, "C" );
	self->parent = parent;

	// Set default libVLC specific properties
	mlt_properties_set( properties, "input_vcodec", "RGBA" );
	mlt_properties_set( properties, "input_acodec", "s16l" );
	mlt_properties_set( properties, "output_vcodec", "mp2v" );
	mlt_properties_set( properties, "output_acodec", "mpga" );
	mlt_properties_set_int( properties, "output_vb", 8000000 );
	mlt_properties_set_int( properties, "output_ab", 128000 );
	mlt_properties_set( properties, "output_dst", arg );
	mlt_properties_set( properties, "output_mux", "ps" );
	mlt_properties_set( properties, "output_access", "file" );

	self->vlc = libvlc_new( 0, NULL );
	assert( self->vlc != NULL );

	// Debug code
	libvlc_log_set( self->vlc, log_cb, NULL );
	pthread_mutex_init( &log_mutex, NULL );

	self->frame_queue = mlt_deque_init( );
	assert( self->frame_queue != NULL );

	pthread_mutex_init( &self->queue_mutex, NULL );

	parent->start = consumer_start;
	parent->stop = consumer_stop;
	parent->is_stopped = consumer_is_stopped;
	parent->close = consumer_close;
	parent->purge = consumer_purge;

	return parent;
}

// Sets up input and output options in VLC,
// initializes media with those options
static void setup_vlc( consumer_libvlc self )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
	const char vcodec[] = "RGBA";
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
	sprintf( audio_string, ":input-slave=imem://cookie=1:cat=1:codec=%s:samplerate=%d:channels=%d:caching=0",
		acodec,
		mlt_properties_get_int( properties, "frequency" ),
		mlt_properties_get_int( properties, "channels" ) );

	// This configures file output
	sprintf( output_string, ":sout=#transcode{"
		"vcodec=%s,fps=%s,width=%d,height=%d,vb=%d,"
		"acodec=%s,channels=%d,samplerate=%d,ab=%d}"
		":standard{access=%s,mux=%s,dst=\"%s\"}",
		mlt_properties_get( properties, "output_vcodec" ),
		mlt_properties_get( properties, "fps" ),
		mlt_properties_get_int( properties, "width" ),
		mlt_properties_get_int( properties, "height" ),
		mlt_properties_get_int( properties, "output_vb" ),
		mlt_properties_get( properties, "output_acodec" ),
		mlt_properties_get_int( properties, "channels" ),
		mlt_properties_get_int( properties, "frequency" ),
		mlt_properties_get_int( properties, "output_ab" ),
		mlt_properties_get( properties, "output_access" ),
		mlt_properties_get( properties, "output_mux" ),
		mlt_properties_get( properties, "output_dst" ) );

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
					 uint32_t *flags, size_t *bufferSize, void **buffer )
{
	consumer_libvlc self = data;
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
	mlt_frame frame = NULL;
	// Whether or not fetched frame need releasing
	int cleanup_frame = 0;
	*buffer = NULL;

	int cookie_int = cookie[ 0 ] - '0';

	// Get data if needed
	pthread_mutex_lock( &self->queue_mutex );

	frame = mlt_deque_pop_front( self->frame_queue );

	// If we got frame from queue, we need to release it later
	if ( frame != NULL )
		cleanup_frame = 1;
	else
		frame = mlt_consumer_get_frame( self->parent );

	if ( cookie_int == AUDIO_COOKIE && self->running )
	{
		assert( frame != NULL );
		mlt_position current_position = mlt_frame_original_position( frame );

		// We terminate imem if we got repeated frame, as this means pause
		if ( current_position == self->audio_position )
		{
			self->running = 0;
			pthread_mutex_unlock( &self->queue_mutex );
			return 1;
		}
		else
		{
			// Update position
			self->audio_position = current_position;

			// This is used to pass frames to imem_release() if they need cleanup
			self->audio_imem_data = NULL;

			mlt_audio_format afmt = mlt_audio_s16;
			double fps = mlt_properties_get_double( properties, "fps" );
			int frequency = mlt_properties_get_int( properties, "frequency" );
			int channels = mlt_properties_get_int( properties, "channels" );
			int samples = mlt_sample_calculator( fps, frequency, self->audio_position );
			double pts_diff = ( double )samples / ( double )frequency * 1000000.0;

			mlt_frame_get_audio( frame, buffer, &afmt, &frequency, &channels, &samples );
			*bufferSize = samples * sizeof( int16_t ) * channels;

			*pts = self->latest_audio_pts + pts_diff + 0.5;
			*dts = *pts;

			self->latest_audio_pts = *pts;

			if ( cleanup_frame )
				self->audio_imem_data = frame;
			else
				mlt_deque_push_back( self->frame_queue, frame );
		}
	}
	else if ( cookie_int == VIDEO_COOKIE && self->running )
	{
		assert( frame != NULL );
		mlt_position current_position = mlt_frame_original_position( frame );

		if ( current_position == self->video_position )
		{
			self->running = 0;
			pthread_mutex_unlock( &self->queue_mutex );
			return 1;
		}
		else
		{
			self->video_position = current_position;

			self->video_imem_data = NULL;

			double fps = mlt_properties_get_double( properties, "fps" );
			double pts_diff = 1.0 / fps * 1000000.0;

			mlt_image_format vfmt = mlt_image_rgb24a;
			int width = mlt_properties_get_int( properties, "width" );
			int height = mlt_properties_get_int( properties, "height" );
			mlt_frame_get_image( frame, ( uint8_t ** )buffer, &vfmt, &width, &height, 0 );
			*bufferSize = mlt_image_format_size( vfmt, width, height, NULL );

			*pts = self->latest_video_pts + pts_diff;
			*dts = *pts;

			self->latest_video_pts = *pts;

			if ( cleanup_frame )
				self->video_imem_data = frame;
			else
				mlt_deque_push_back( self->frame_queue, frame );
			}
	}
	else if ( self->running )
	{
		// Invalid cookie
		assert( 0 );
	}
	pthread_mutex_unlock( &self->queue_mutex );

	assert( frame != NULL );
	if ( *buffer == NULL )
		return 1;

	return 0;
}

static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer )
{
	consumer_libvlc self = data;

	int cookie_int = cookie[ 0 ] - '0';

	if ( cookie_int == VIDEO_COOKIE && self->running )
	{
		if ( self->video_imem_data )
		{
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
			mlt_frame frame = self->video_imem_data;
			mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
			self->video_imem_data = NULL;
		}
	}
	else if ( cookie_int == AUDIO_COOKIE && self->running )
	{
		if ( self->audio_imem_data )
		{
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
			mlt_frame frame = self->audio_imem_data;
			mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
			self->audio_imem_data = NULL;

		}
	}
	else if ( self->running )
	{
		// Invalid cookie
		assert( 0 );
	}
}

static void mp_callback( const struct libvlc_event_t *evt, void *data )
{
	consumer_libvlc self = data;
	assert( self != NULL );

	switch ( evt->type )
	{
		case libvlc_MediaPlayerStopped:
			self->running = 0;
			break;

		default:
			assert( 0 );
	}
}

static int consumer_start( mlt_consumer parent )
{
	int err;

	consumer_libvlc self = parent->child;
	assert( self != NULL );

	if ( consumer_is_stopped( parent ) )
	{
		// Free all previous resources
		if ( self->media_player )
		{
			libvlc_media_player_release( self->media_player );
			self->media_player = NULL;
		}
		if ( self->media )
		{
			libvlc_media_release( self->media );
			self->media = NULL;
		}

		// Apply properties to new media
		setup_vlc( self );
		self->media_player = libvlc_media_player_new_from_media( self->media );
		assert( self->media_player != NULL );
		self->mp_manager = libvlc_media_player_event_manager( self->media_player );
		assert( self->mp_manager != NULL );
		libvlc_event_attach( self->mp_manager, libvlc_MediaPlayerStopped, &mp_callback, self );

		// Reset play heads
		self->video_position = INITIAL_POSITION;
		self->audio_position = INITIAL_POSITION;

		// Run media player
		self->running = 1;
		err = libvlc_media_player_play( self->media_player );

		// If we failed to play, we're not running
		if ( err )
			self->running = 0;

		return err;
	}
	return 1;
}

static int consumer_stop( mlt_consumer parent )
{
	consumer_libvlc self = parent->child;
	assert( self != NULL );

	if ( self->media_player )
	{
		self->running = 0;
		libvlc_media_player_stop( self->media_player );
	}

	// Reset pts counters
	self->latest_video_pts = 0;
	self->latest_audio_pts = 0;

	return 0;
}

static int consumer_is_stopped( mlt_consumer parent )
{
	consumer_libvlc self = parent->child;
	assert( self != NULL );

	if ( self->media_player )
	{
		return !self->running;
	}

	return 1;
}

static void consumer_purge( mlt_consumer parent )
{
	// We do nothing here, we purge on stop()
}

static void consumer_close( mlt_consumer parent )
{
	if ( parent == NULL )
	{
		return;
	}

	consumer_libvlc self = parent->child;

	if ( self != NULL )
	{
		consumer_stop( parent );

		if ( self->media_player )
			libvlc_media_player_release( self->media_player );

		if ( self->media )
			libvlc_media_release( self->media );

		if ( self->vlc )
			libvlc_release( self->vlc );

		pthread_mutex_destroy( &self->queue_mutex );
		free( self );
	}

	parent->close = NULL;
	mlt_consumer_close( parent );
}
