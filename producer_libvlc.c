#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <locale.h>

#include "frame_cache.h"
#include "buffer_queue.h"

#define SEEK_THRESHOLD 25

typedef struct producer_libvlc_s *producer_libvlc;

struct producer_libvlc_s
{
	mlt_producer parent;
	libvlc_instance_t *vlc;
	libvlc_media_t *media;
	libvlc_media_player_t *media_player;

	// Flag for cleanup
	int terminating;

	buffer_queue bqueue;
	frame_cache cache;
	pthread_mutex_t cache_mutex;
	pthread_cond_t cache_cond;
	int64_t seek_request_timestamp;
	mlt_position seek_request_position;
	int during_seek;
	mlt_position smem_position;

	// This is for holding VLC buffer metadata while it's running
	unsigned int channels;
};

static void log_cb( void *data, int vlc_level, const libvlc_log_t *ctx, const char *fmt, va_list args )
{
	if ( data == NULL )
		return;

	producer_libvlc self = data;

	int mlt_level;
	switch ( vlc_level )
	{
		case LIBVLC_DEBUG:
			mlt_level = MLT_LOG_DEBUG;
			break;
		case LIBVLC_NOTICE:
			mlt_level = MLT_LOG_INFO;
			break;
		case LIBVLC_WARNING:
			mlt_level = MLT_LOG_WARNING;
			break;
		case LIBVLC_ERROR:
		default:
			mlt_level = MLT_LOG_FATAL;
	}

	// In order to get readable output from MLT default log handler,
	// we need to end our message with newline, VLC doesn't do that
	size_t fmt_len = strlen( fmt );
	// + \n + \0
	char *fmt_nl = calloc( fmt_len + 1 + 1, sizeof( char ) );
	if ( fmt_nl == NULL )
		return;

	strcat( fmt_nl, fmt );
	strcat( fmt_nl, "\n" );

	mlt_vlog( MLT_PRODUCER_SERVICE( self->parent ), mlt_level, fmt_nl, args );

	free( fmt_nl );
}

// Forward references
static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index );
static void collect_stream_data( producer_libvlc self );
static int setup_smem( producer_libvlc self );
static void producer_close( mlt_producer parent );
static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size );
static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts );
static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size );
static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts );
static int setup_vlc( producer_libvlc self );
static void cleanup_vlc( producer_libvlc self );
static void smem_pack_frames_or_block( producer_libvlc self );

mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *file )
{
	// If we dealt with allocating mlt_profile, we need to clean it up in case of fail
	int profile_allocated = 0;

	// If we receive a NULL file argument, we've got nothing to do
	if ( file == NULL )
		return NULL;

	// If we receive a NULL profile we can initialize a default one though
	if ( profile == NULL )
	{
		profile = mlt_profile_init( NULL );
		// But if we fail here too, we've got nothing to do as well...
		if ( profile == NULL )
			return NULL;
		profile_allocated = 1;
	}

	mlt_producer producer = NULL;
	producer_libvlc self = NULL;

	// Construct the producer
	self = calloc( 1, sizeof( struct producer_libvlc_s ) );
	producer = calloc( 1, sizeof( struct mlt_producer_s ) );

	if ( self == NULL || producer == NULL ) goto cleanup;
	if ( mlt_producer_init( producer, self ) != 0 ) goto cleanup;

	// Set default properties
	mlt_properties_set( MLT_PRODUCER_PROPERTIES( producer ), "resource", file );
	mlt_properties_set_data( MLT_PRODUCER_PROPERTIES( producer ), "_profile", profile, 0, NULL, NULL );
	mlt_properties_set_double( MLT_PRODUCER_PROPERTIES( producer ), "aspect_ratio", mlt_profile_sar( profile ) );
	mlt_properties_set_int( MLT_PRODUCER_PROPERTIES( producer ), "frame_cache_size", 25 );
	// This is needed because VLC uses dot as floating point separator
	mlt_properties_set_lcnumeric( MLT_PRODUCER_PROPERTIES( producer ), "C" );
	// Default audio settings
	mlt_properties_set_int( MLT_PRODUCER_PROPERTIES( producer ), "channels", 2 );
	mlt_properties_set_int( MLT_PRODUCER_PROPERTIES( producer ), "frequency", 48000 );

	// Set libVLC's producer parent
	self->parent = producer;

	// Set destructor
	producer->close = producer_close;

	// Override virtual function for getting frames
	producer->get_frame = producer_get_frame;

	// Initialize mutexes and conds
	pthread_mutex_init( &self->cache_mutex, NULL );
	pthread_cond_init( &self->cache_cond, NULL );

	// Initialize all neded VLC objects (or cleanup on fail)
	if ( setup_vlc( self ) ) goto cleanup;

	return producer;

cleanup:
	if ( profile_allocated ) mlt_profile_close( profile );
	free( self );
	free( producer );

	return NULL;
}

static int setup_vlc( producer_libvlc self )
{
	if ( self == NULL )
		return 1;

	// Get producer's properties
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	// Get resource to open
	char *file = mlt_properties_get( properties, "resource" );
	if ( file == NULL )
		return 1;

	// Initialize VLC instance
	self->vlc = libvlc_new( 0, NULL );
	if ( self->vlc == NULL ) goto cleanup;

	// Pass logs to MLT
	libvlc_log_set( self->vlc, log_cb, self );

	// Initialize VLC media
	self->media = libvlc_media_new_path( self->vlc, file );
	if ( self->media == NULL ) goto cleanup;

	// Collect stream metadata
	collect_stream_data( self );

	// Setup libVLC smem
	if ( setup_smem( self ) ) goto cleanup;

	// Create smem media player
	self->media_player = libvlc_media_player_new_from_media( self->media );
	if ( self->media_player == NULL ) goto cleanup;

	// Release media now, that we don't need it
	libvlc_media_release( self->media );
	self->media = NULL;

	// Create buffer_queue and frame_cache
	if ( self->bqueue == NULL )
	{
		mlt_image_format vfmt = mlt_properties_get_int( properties, "_mlt_image_format" );
		mlt_audio_format afmt = mlt_properties_get_int( properties, "_mlt_audio_format" );
		int channels = mlt_properties_get_int( properties, "_channels" );
		int samplerate = mlt_properties_get_int( properties, "_frequency" );
		self->bqueue = buffer_queue_init( MLT_PRODUCER_SERVICE( self->parent ), vfmt, afmt, channels, samplerate );
		if ( self->bqueue == NULL ) goto cleanup;
	}
	else
	{
		buffer_queue_purge( self->bqueue );
	}

	if ( self->cache == NULL )
	{
		int frame_cache_size = mlt_properties_get_int( properties, "frame_cache_size" );
		mlt_properties_set_int( properties, "_frame_cache_size", frame_cache_size );
		self->cache = frame_cache_init( frame_cache_size );
		if ( self->cache == NULL ) goto cleanup;
	}
	else
	{
		frame_cache_purge( self->cache );
	}

	// Start smem
	libvlc_media_player_play( self->media_player );

	// All went well
	return 0;

cleanup:
	if ( self->bqueue ) buffer_queue_close( self->bqueue );
	cleanup_vlc( self );
	return 1;
}

static void cleanup_vlc( producer_libvlc self )
{
	if ( self == NULL )
		return;

	if ( self->vlc )
	{
		libvlc_release( self->vlc );
		self->vlc = NULL;
	}
	if ( self->media )
	{
		libvlc_media_release( self->media );
		self->media = NULL;
	}
	if ( self->media_player )
	{
		libvlc_media_player_release( self->media_player );
		self->media_player = NULL;
	}
}

static void collect_stream_data( producer_libvlc self )
{
	if ( self->media == NULL )
	{
		return;
	}

	int track_i;
	unsigned int nb_tracks;
	libvlc_media_track_t *track;
	libvlc_video_track_t *v_track;

	// Get handles to necessary objects
	mlt_properties p = MLT_PRODUCER_PROPERTIES( self->parent );
	libvlc_media_t *media = self->media;

	// Collect media metadata
	libvlc_media_parse( media );

	// Read and save media metadata to properties
	libvlc_media_track_t **tracks;
	nb_tracks = libvlc_media_tracks_get( media, &tracks );

	// Search for default video track to fetch metadata
	for ( track_i = 0; track_i < nb_tracks; track_i++ ) {
		track = tracks[ track_i ];

		// We pick first video track as the default one
		if ( track->i_type == libvlc_track_video ) {
			v_track = track->video;
			// This sets metadata, which can be useful for creating auto-profile
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

static int setup_smem( producer_libvlc self )
{
	mlt_profile profile = mlt_service_profile( MLT_PRODUCER_SERVICE( self->parent ) );
	if ( profile == NULL )
	{
		mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_ERROR, "%s\n", "setup_smem: Could not fetch mlt_profile\n" );
		return 1;
	}

	char vcodec[] = "RV24";
	char acodec[] = "s16l";

	mlt_properties p = MLT_PRODUCER_PROPERTIES( self->parent );

	// We use properties to make sure VLC has consistent data through its runtime
	// (if we used producer's profile someone could have changed it during VLC
	// runtime which would cause VLC to have incorrect data during frame rendering)
	mlt_properties_set_double( p, "_fps", mlt_profile_fps( profile ) );
	mlt_properties_set_int( p, "_width", profile->width );
	mlt_properties_set_int( p, "_height", profile->height );
	mlt_properties_set_int( p, "_channels", mlt_properties_get_int( p, "channels" ) );
	mlt_properties_set_int( p, "_frequency", mlt_properties_get_int( p, "frequency" ) );
	mlt_properties_set_int( p, "_mlt_audio_format", mlt_audio_s16 );
	mlt_properties_set_int( p, "_mlt_image_format", mlt_image_rgb24 );

	// Build smem options string
	char smem_options[ 1000 ];
	sprintf( smem_options,
			 ":sout=#transcode{"
			 "vcodec=%s,"
			 "fps=%s,"
			 "width=%d,"
			 "height=%d,"
			 "acodec=%s,"
			 "channels=%d,"
			 "samplerate=%d,"
			 "}:smem{"
			 "no-time-sync,"
			 "audio-prerender-callback=%" PRIdPTR ","
			 "audio-postrender-callback=%" PRIdPTR ","
			 "video-prerender-callback=%" PRIdPTR ","
			 "video-postrender-callback=%" PRIdPTR ","
			 "audio-data=%" PRIdPTR ","
			 "video-data=%" PRIdPTR ","
			 "}",
			 vcodec,
			 mlt_properties_get( p, "_fps" ),
			 mlt_properties_get_int( p, "_width" ),
			 mlt_properties_get_int( p, "_height" ),
			 acodec,
			 mlt_properties_get_int( p, "_channels" ),
			 mlt_properties_get_int( p, "_frequency" ),
			 (intptr_t)(void*)&audio_prerender_callback,
			 (intptr_t)(void*)&audio_postrender_callback,
			 (intptr_t)(void*)&video_prerender_callback,
			 (intptr_t)(void*)&video_postrender_callback,
			 (intptr_t)(void*)self,
			 (intptr_t)(void*)self );

	// Supply smem options to libVLC
	libvlc_media_add_option( self->media, smem_options );

	return 0;
}

// WARNING: Lock cache_mutex before calling this function
static void smem_pack_frames_or_block( producer_libvlc self )
{
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	int cache_size = mlt_properties_get_int( properties, "_frame_cache_size" );

	mlt_position earliest_frame_pos = frame_cache_earliest_frame_position( self->cache );
	mlt_position latest_frame_pos = frame_cache_latest_frame_position( self->cache );

	// Block if rendering packing new frame would erase the one we need from cache
	while ( earliest_frame_pos == mlt_producer_position( self->parent ) && latest_frame_pos - earliest_frame_pos + 1 == cache_size && !self->during_seek )
	{
		pthread_cond_wait( &self->cache_cond, &self->cache_mutex );
	}

	if ( !self->during_seek )
	{
		mlt_frame frame = buffer_queue_pack_frame( self->bqueue, self->smem_position );
		if ( frame != NULL )
		{
			self->smem_position++;
			frame_cache_put_frame( self->cache, frame );
		}
	}
}

static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size )
{
	producer_libvlc self = p_audio_data;

	mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "%s\n", "audio_prerender_callback: start\n" );

	// If we're terminating, we need to abort render
	if ( self->terminating )
		*pp_pcm_buffer = NULL;
	else
		*pp_pcm_buffer = mlt_pool_alloc( size * sizeof( uint8_t ) );

	return;
}

static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts )
{
	producer_libvlc self = p_audio_data;

	mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "%s\n", "audio_postrender_callback: start\n" );

	buffer_queue_insert_audio_buffer( self->bqueue, p_pcm_buffer, size );

	pthread_mutex_lock( &self->cache_mutex );
	// Check if we have seeked already
	if ( self->during_seek )
	{
		int64_t vlc_timestamp = libvlc_media_player_get_time( self->media_player );
		mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "audio_postrender_callback: now seeking. Current timestamp %" PRId64 "\n", vlc_timestamp );
		if ( vlc_timestamp == self->seek_request_timestamp )
		{
			buffer_queue_purge( self->bqueue );
			frame_cache_purge( self->cache );
			self->during_seek = 0;
			self->smem_position = self->seek_request_position;
		}
	}

	// If we're not seeking, we try to pack buffer into frame
	if ( !self->during_seek )
		smem_pack_frames_or_block( self );

	// Broadcast to MLT, because frame_cache may now have the frame it's requesting for
	pthread_cond_broadcast( &self->cache_cond );

	pthread_mutex_unlock( &self->cache_mutex );
}

static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size )
{
	producer_libvlc self = data;

	mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "%s\n", "video_prerender_callback: start\n" );

	// If we're terminating, we need to abort render
	if ( self->terminating )
		*p_buffer = NULL;
	else
		*p_buffer = mlt_pool_alloc( size * sizeof( uint8_t ) );

	return;
}

static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts )
{
	producer_libvlc self = data;

	mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "%s\n", "video_postrender_callback: start\n" );

	buffer_queue_insert_video_buffer( self->bqueue, buffer, size );

	pthread_mutex_lock( &self->cache_mutex );
	// Check if we have seeked already
	if ( self->during_seek )
	{
		int64_t vlc_timestamp = libvlc_media_player_get_time( self->media_player );
		mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "video_postrender_callback: now seeking. Current timestamp %" PRId64 "\n", vlc_timestamp );
		if ( vlc_timestamp == self->seek_request_timestamp )
		{
			buffer_queue_purge( self->bqueue );
			frame_cache_purge( self->cache );
			self->during_seek = 0;
			self->smem_position = self->seek_request_position;
		}
	}

	// If we're not seeking, we try to pack buffer into frame
	if ( !self->during_seek )
		smem_pack_frames_or_block( self );

	// Broadcast to MLT, because frame_cache may now have the frame it's requesting for
	pthread_cond_broadcast( &self->cache_cond );

	pthread_mutex_unlock( &self->cache_mutex );
}

static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame_ptr, int index )
{
	// Get handle to libVLC's producer
	producer_libvlc self = producer->child;

	pthread_mutex_lock( &self->cache_mutex );

	// Aquire current position
	mlt_position current_position = mlt_producer_position( producer );
	double fps = mlt_properties_get_double( MLT_PRODUCER_PROPERTIES( producer ), "_fps" );

	mlt_position earliest_frame_pos =
		frame_cache_earliest_frame_position( self->cache );
	mlt_position latest_frame_pos =
		frame_cache_latest_frame_position( self->cache );

	// Seek and wait for seek if needed
	if ( earliest_frame_pos > current_position || current_position - latest_frame_pos > SEEK_THRESHOLD )
	{
		mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "producer_get_frame: Seeking to pos %d\n", current_position );
		self->during_seek = 1;
		self->seek_request_position = current_position;
		self->seek_request_timestamp = 1000.0 * current_position / fps + 0.5;
		mlt_log( MLT_PRODUCER_SERVICE( self->parent ), MLT_LOG_DEBUG, "producer_get_frame: Requested timestamp is %" PRId64 "\n", self->seek_request_timestamp );
		libvlc_media_player_set_time( self->media_player, self->seek_request_timestamp );
		while ( self->during_seek )
		{
			pthread_cond_broadcast( &self->cache_cond );
			pthread_cond_wait( &self->cache_cond, &self->cache_mutex );
		}
	}

	mlt_frame frame = NULL;
	while ( !( frame = frame_cache_get_frame( self->cache, current_position ) ) )
	{
		pthread_cond_wait( &self->cache_cond, &self->cache_mutex );
	}

	*frame_ptr = frame;

	// Prepare next frame
	mlt_producer_prepare_next( producer );

	pthread_cond_broadcast( &self->cache_cond );
	pthread_mutex_unlock( &self->cache_mutex );
	return 0;
}

static void producer_close( mlt_producer parent )
{
	if ( parent != NULL ) {
		producer_libvlc self = parent->child;

		// Stop smem threads
		pthread_mutex_lock( &self->cache_mutex );
		self->terminating = 1;
		pthread_cond_broadcast( &self->cache_cond );
		pthread_mutex_unlock( &self->cache_mutex );
		libvlc_media_player_stop( self->media_player );

		// Release libVLC objects
		libvlc_media_player_release( self->media_player );
		libvlc_media_release( self->media );
		libvlc_release( self->vlc );

		// Clear mutexes and conds
		pthread_mutex_destroy( &self->cache_mutex );
		pthread_cond_destroy( &self->cache_cond );

		// Free allocated memory for libvlc_producer
		free( self );

		// Call overriden destructor
		parent->close = NULL;
		mlt_producer_close( parent );
	}
}
