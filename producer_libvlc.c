#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

typedef struct producer_libvlc_s *producer_libvlc;

struct producer_libvlc_s
{
	mlt_producer parent;
	libvlc_instance_t *libvlc_instance;
	libvlc_media_t *media;
	libvlc_media_player_t *media_player;
	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	uint8_t *video_buffer;
};

// Forward references
static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index );
static void collect_stream_data( producer_libvlc self, char *file );
static void setup_smem( producer_libvlc self );
static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size );
static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts );
static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size );
static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts );

mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *file )
{
	int cleanup_flag = 0;
	mlt_producer producer = NULL;

	// Check that we have a non-NULL argument
	if ( file )
	{
		// Construct the producer
		producer_libvlc self = calloc( 1, sizeof( struct producer_libvlc_s ) );
		producer = calloc( 1, sizeof( struct mlt_producer_s ) );

		// Initialise it
		if ( mlt_producer_init( producer, self ) == 0 )
		{
			self->parent = producer;

			// Initialize libVLC
			self->libvlc_instance = libvlc_new( 0, NULL );

			// If initialization was successful, we can proceed
			if ( self->libvlc_instance != NULL )
			{
				self->media = libvlc_media_new_path( self->libvlc_instance, file );

				// Initialize media and metadata
				collect_stream_data( self, file );

				// Setup libVLC smem
				setup_smem( self );
				self->media_player = libvlc_media_player_new_from_media( self->media );
				libvlc_media_player_play( self->media_player );

				pthread_mutex_init( &self->video_mutex, NULL );
				pthread_mutex_init( &self->audio_mutex, NULL );

				// Override virtual function for getting frames
				producer->get_frame = producer_get_frame;

				// Connect libVLC producer to its parent class
				producer->child = self;
			}
			// Without libVLC, our producer is useless
			else
			{
				cleanup_flag = 1;
			}
		}
		// Initialization wasn't successful - we must clear allocated structs
		else
		{
			cleanup_flag = 1;
		}

		// If flag was enbled, do a cleanup routine
		if ( cleanup_flag )
		{
			free( self );
			free( producer );
			producer = NULL;
		}
	}

	return producer;
}

static void collect_stream_data( producer_libvlc self, char *file )
{
	int track_i;
	unsigned int nb_tracks;
	libvlc_media_track_t *track;
	libvlc_video_track_t *v_track;

	// Initialize media
	self->media = libvlc_media_new_path( self->libvlc_instance, file );
	// If it wasn't successful, we can't collect metadata
	if (self->media == NULL)
	{
		return;
	}

	// Get handles to necessary objects
	mlt_properties p = MLT_PRODUCER_PROPERTIES( self->parent );
	libvlc_media_t *media = self->media;

	// Collect media metadata
	libvlc_media_parse( media );

	// Read and save media metadata to properties
	libvlc_media_track_t **tracks;
	nb_tracks = libvlc_media_tracks_get( media, &tracks );
	for ( track_i = 0; track_i < nb_tracks; track_i++ ) {
		track = tracks[ track_i ];

		// We pick first video track as the most important one
		if ( track->i_type == libvlc_track_video ) {
			v_track = track->video;
			mlt_properties_set_int( p, "meta.media.width", v_track->i_width );
			mlt_properties_set_int( p, "meta.media.height", v_track->i_height );
			mlt_properties_set_int( p, "meta.media.frame_rate_num", v_track->i_frame_rate_num );
			mlt_properties_set_int( p, "meta.media.frame_rate_den", v_track->i_frame_rate_den );
			mlt_properties_set_int( p, "meta.media.sample_aspect_num", v_track->i_sar_num );
			mlt_properties_set_int( p, "meta.media.sample_aspect_den", v_track->i_sar_den );
			break;
		}
	}
	libvlc_media_tracks_release( tracks, nb_tracks );
}

static void setup_smem( producer_libvlc self )
{
	int width, height, size, i;
	float fps;
	// Needed to make sure it is delimited by dot
	char fps_string[ 1000 ];

	char vcodec[] = "I420";
	char acodec[] = "u8 ";

	mlt_properties p = MLT_PRODUCER_PROPERTIES( self->parent );

	// Collect metadata to local variables
	width = mlt_properties_get_int( p, "meta.media.width" );
	height = mlt_properties_get_int( p, "meta.media.height" );
	fps = mlt_properties_get_int( p, "meta.media.frame_rate_num" )
			/ mlt_properties_get_int( p, "meta.media.frame_rate_den" );

	size = snprintf( fps_string, 1000, "%f", fps);
	for (i = 0; i < size; i++)
	{
		if (fps_string[ i ] == ',')
		{
			fps_string[ i ] = '.';
		}
	}

	// Build smem options string
	char smem_options[ 1000 ];
	sprintf( smem_options,
			 ":sout=#transcode{"
			 "vcodec=%s,"
			 "fps=%s,"
			 "width=%d,"
			 "height=%d,"
			 "acodec=%s,"
			 "}:smem{"
			 "no-time-sync,"
			 "audio-prerender-callback=%lld,"
			 "audio-postrender-callback=%lld,"
			 "video-prerender-callback=%lld,"
			 "video-postrender-callback=%lld,"
			 "}",
			 vcodec, fps_string, width, height, acodec,
			 (long long int)(intptr_t)(void*)&audio_prerender_callback,
			 (long long int)(intptr_t)(void*)&audio_postrender_callback,
			 (long long int)(intptr_t)(void*)&video_prerender_callback,
			 (long long int)(intptr_t)(void*)&video_postrender_callback );

	// Supply smem options to libVLC
	libvlc_media_add_option( self->media, smem_options );
}

static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size )
{
	/* TODO */
	printf("audio lock\n");
	uint8_t *buffer = malloc( size * sizeof( uint8_t ) );
	*pp_pcm_buffer = buffer;
}

static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts )
{
	/* TODO */
	printf("audio unlock\n");
	free( p_pcm_buffer );
}

static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size )
{
	/* TODO */
	printf("video lock\n");
	uint8_t *buffer = malloc( size * sizeof( uint8_t ) );
	*p_buffer = buffer;
}

static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts )
{
	/* TODO */
	printf("video unlock\n");
	free( buffer );
}


static int producer_get_image( mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable )
{
	/* TODO */
	return 0;
}


static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
	/* TODO */

	producer_libvlc self = producer->child;

	// Access the private data (producer is needed to get profile)
	mlt_service service = MLT_PRODUCER_SERVICE( producer );

	*frame = mlt_frame_init( service );
	mlt_properties frame_properties = mlt_frame_properties( *frame );

	mlt_producer_prepare_next( producer );

	return 0;
}
