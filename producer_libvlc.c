#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <locale.h>

#define MAX_CACHE_SIZE 100
#define AUDIO_SAMPLE_SIZE sizeof( int16_t )

// To the reviewer:
// Assertions in code will be deleted later,
// these are here to assure that I have proper
// understanding of libVLC (and should be reviewed as well).

pthread_mutex_t log_mutex;

void log_cb(void *data, int level, const libvlc_log_t *ctx, const char *fmt, va_list args)
{
	pthread_mutex_lock(&log_mutex);
	printf("VLC LOG: ");
	vprintf(fmt, args);
	printf("\n");
	pthread_mutex_unlock(&log_mutex);
}

typedef struct producer_libvlc_s *producer_libvlc;
typedef struct a_cache_item_s *a_cache_item;
typedef struct v_cache_item_s *v_cache_item;

// Video cache item
struct v_cache_item_s
{
	uint8_t *buffer;
	size_t size;
	int bpp;
	int width;
	int height;
};

// Audio cache item
struct a_cache_item_s
{
	// buffer of samples
	void *buffer;
	// number of samples per channel
	size_t samples;
	// number of channels
	int channels;
	// sample rate
	int sample_rate;
	// bits per sample
	int bps;
};

struct producer_libvlc_s
{
	mlt_producer parent;
	libvlc_instance_t *libvlc_instance;
	libvlc_media_t *media;
	libvlc_media_player_t *media_player;
	mlt_deque v_cache;
	int v_cache_producers;
	int v_cache_consumers;
	pthread_mutex_t v_cache_mutex;
	pthread_cond_t v_cache_producer_cond;
	pthread_cond_t v_cache_consumer_cond;
	mlt_deque a_cache;
	mlt_position last_frame_requested;
	int a_cache_producers;
	int a_cache_consumers;
	pthread_mutex_t a_cache_mutex;
	pthread_cond_t a_cache_producer_cond;
	pthread_cond_t a_cache_consumer_cond;
	// temporary audio buffer for samples
	void *ta_buffer;
	// size of ta_buffer
	size_t ta_buffer_size;
	mlt_position current_audio_frame;
	// Flag for cleanup
	int terminating;
	// debug
	int audio_locks;
	int audio_unlocks;
	int image_gots;
	int audio_gots;
};

// Forward references
static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index );
static void set_properties( producer_libvlc self, mlt_profile profile );
static void collect_stream_data( producer_libvlc self, char *file );
static void setup_smem( producer_libvlc self );
static void producer_close( mlt_producer parent );
static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size );
static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts );
static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size );
static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts );

mlt_producer producer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, char *file )
{
	// If we receive a NULL file argument, we've got nothing to do
	if ( file == NULL )
		return NULL;

	mlt_producer producer = NULL;
	producer_libvlc self = NULL;
	int libvlc_initialized = 0;
	int media_initialized = 0;
	int media_player_initialized = 0;

	// Construct the producer
	self = calloc( 1, sizeof( struct producer_libvlc_s ) );
	producer = calloc( 1, sizeof( struct mlt_producer_s ) );

	if ( self == NULL || producer == NULL ) goto cleanup;
	if ( mlt_producer_init( producer, self ) != 0 ) goto cleanup;

	// Set libVLC's producer parent
	self->parent = producer;

	// Set destructor
	producer->close = producer_close;

	// Initialize libVLC
	self->libvlc_instance = libvlc_new( 0, NULL );
	if ( self->libvlc_instance == NULL ) goto cleanup;
	libvlc_initialized = 1;

	// Left for debugging
	libvlc_log_set(self->libvlc_instance, log_cb, NULL);
	pthread_mutex_init(&log_mutex, NULL);

	// Initialize media
	self->media = libvlc_media_new_path( self->libvlc_instance, file );
	if ( self->media == NULL ) goto cleanup;
	media_initialized = 1;

	// Override virtual function for getting frames
	producer->get_frame = producer_get_frame;

	// Initialize cache and its mutexes
	self->v_cache = mlt_deque_init( );
	self->a_cache = mlt_deque_init( );

	pthread_mutex_init( &self->v_cache_mutex, NULL );
	pthread_cond_init( &self->v_cache_producer_cond, NULL );
	pthread_cond_init( &self->v_cache_consumer_cond, NULL );
	pthread_mutex_init( &self->a_cache_mutex, NULL );
	pthread_cond_init( &self->a_cache_producer_cond, NULL );
	pthread_cond_init( &self->a_cache_consumer_cond, NULL );

	// Collect stream metadata
	collect_stream_data( self, file );

	// Set properties to make all profile info available to all functions thorugh them
	set_properties( self, profile );

	// Setup libVLC smem
	setup_smem( self );

	// Create smem media player
	self->media_player = libvlc_media_player_new_from_media( self->media );
	if ( self->media_player == NULL ) goto cleanup;
	media_player_initialized = 1;

	// TODO: Should this go somewhere else?
	libvlc_media_player_play( self->media_player );

	return producer;

cleanup:
	if ( libvlc_initialized ) libvlc_release( self->libvlc_instance );
	if ( media_initialized ) libvlc_media_release( self->media );
	if ( media_player_initialized ) libvlc_media_player_release( self->media_player );
	free( self );
	free( producer );
	return NULL;
}

static void collect_stream_data( producer_libvlc self, char *file )
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

static void set_properties( producer_libvlc self, mlt_profile profile )
{
	// TODO: Don't know what to do about that yet
	assert( profile != NULL );

	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	// Smem uses comma as floating point separator
	mlt_properties_set_lcnumeric( properties, "C" );

	mlt_properties_set_int( properties, "width", profile->width );

	mlt_properties_set_int( properties, "height", profile->height );
	mlt_properties_set_double( properties, "fps", mlt_profile_fps( profile ) );

	// Default audio settings
	mlt_properties_set_int( properties, "channels", 2 );
	mlt_properties_set_int( properties, "frequency", 48000 );
}

static void setup_smem( producer_libvlc self )
{
	char vcodec[] = "RV24";
	char acodec[] = "s16l";

	mlt_properties p = MLT_PRODUCER_PROPERTIES( self->parent );

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
			 mlt_properties_get( p, "fps" ),
			 mlt_properties_get_int( p, "width" ),
			 mlt_properties_get_int( p, "height" ),
			 acodec,
			 mlt_properties_get_int( p, "channels" ),
			 mlt_properties_get_int( p, "samplerate" ),
			 (intptr_t)(void*)&audio_prerender_callback,
			 (intptr_t)(void*)&audio_postrender_callback,
			 (intptr_t)(void*)&video_prerender_callback,
			 (intptr_t)(void*)&video_postrender_callback,
			 (intptr_t)(void*)self,
			 (intptr_t)(void*)self );

	// Supply smem options to libVLC
	libvlc_media_add_option( self->media, smem_options );
}

static void audio_prerender_callback( void* p_audio_data, uint8_t** pp_pcm_buffer, size_t size )
{
	// Flag for termination purposes
	int mutex_locked = 0;

	producer_libvlc self = p_audio_data;

	// If we're terminating, we need to abort render
	if ( self->terminating )
		goto terminate;

	// Lock cache mutex
	pthread_mutex_lock( &self->a_cache_mutex );
	mutex_locked = 1;

	// Wait for space in cache
	self->a_cache_producers++;
	while ( mlt_deque_count( self->a_cache ) == MAX_CACHE_SIZE )
	{
		pthread_cond_wait( &self->a_cache_producer_cond, &self->a_cache_mutex );

		// Terminate function could have woken us up
		if ( self->terminating )
			goto terminate;
	}
	self->a_cache_producers--;

	// Allocate space for buffer and proceed with rendering
	uint8_t *buffer = mlt_pool_alloc( size * sizeof( uint8_t ) );
	*pp_pcm_buffer = buffer;

	return;

terminate:
	*pp_pcm_buffer = NULL;
	// If we own the mutex, we need to release it for destruction
	if ( mutex_locked )
		pthread_mutex_unlock( &self->a_cache_mutex );
	return;
}

static void audio_postrender_callback( void* p_audio_data, uint8_t* p_pcm_buffer, unsigned int channels,
									   unsigned int rate, unsigned int nb_samples, unsigned int bits_per_sample,
									   size_t size, int64_t pts )
{
	assert( ( nb_samples * AUDIO_SAMPLE_SIZE * channels ) <= size );

	producer_libvlc self = p_audio_data;
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	double fps = mlt_properties_get_double( properties, "fps" );

	// This is how many samples per channel we need for the current audio frame
	int needed_samples = mlt_sample_calculator( fps, rate, self->current_audio_frame );
	// Size of a complete single sample is equal to AUDIO_SAMPLE_SIZE * channels
	int buffered_samples = self->ta_buffer_size / ( AUDIO_SAMPLE_SIZE * channels );
	// How many samples have we computed
	int calculated_samples = nb_samples + buffered_samples;
	// We have all data needed to complete a frame
	if ( calculated_samples >= needed_samples )
	{
		size_t samples_to_buffer = calculated_samples - needed_samples;
		size_t new_ta_buffer_size = samples_to_buffer * AUDIO_SAMPLE_SIZE * channels;
		size_t frame_samples_size = needed_samples * AUDIO_SAMPLE_SIZE * channels;
		// Allocate buffer for frame samples
		uint8_t *frame_samples = mlt_pool_alloc( frame_samples_size * sizeof( uint8_t ) );
		// And for the new temp audio buffer
		uint8_t *new_ta_buffer = mlt_pool_alloc( new_ta_buffer_size * sizeof( uint8_t ) );
		// Copy frame samples to buffer and initialize new temp buffer
		if ( buffered_samples < needed_samples ) // We need to use both buffers
		{
			// Copy entire temp audio buffer contents
			memcpy( frame_samples, self->ta_buffer, self->ta_buffer_size );
			// Copy samples from pcm buffer that we still need
			memcpy( frame_samples + self->ta_buffer_size, p_pcm_buffer,
				( needed_samples - buffered_samples ) * AUDIO_SAMPLE_SIZE * channels );
			// Copy remaining samples to new temp audio buffer
			memcpy( new_ta_buffer,
				p_pcm_buffer + ( needed_samples - buffered_samples ) * AUDIO_SAMPLE_SIZE * channels,
				new_ta_buffer_size );
		}
		else // We need to use samples from temp audio buffer only
		{
			// Copy all needed samples from temp audio buffer
			memcpy( frame_samples, self->ta_buffer, frame_samples_size );
			// Copy remaining samples from both buffers to new temp buffer
			memcpy( new_ta_buffer, self->ta_buffer + frame_samples_size,
				self->ta_buffer_size - frame_samples_size );
			memcpy( new_ta_buffer + ( self->ta_buffer_size - frame_samples_size ), p_pcm_buffer,
				nb_samples * AUDIO_SAMPLE_SIZE * channels );
		}

		// We don't need those buffers anymore
		mlt_pool_release( self->ta_buffer );
		mlt_pool_release( p_pcm_buffer );

		// Move our new temp audio buffer to producer
		self->ta_buffer = new_ta_buffer;
		self->ta_buffer_size = new_ta_buffer_size;

		// Move samples to cache
		a_cache_item a_item = calloc( 1, sizeof( struct a_cache_item_s ) );
		a_item->buffer = frame_samples;
		a_item->samples = needed_samples;
		a_item->sample_rate = rate;
		a_item->channels = channels;

		mlt_deque_push_back( self->a_cache, a_item );

		// We will be computing next frame now
		self->current_audio_frame++;

		// If there are consumers waiting, signal them
		if ( self->a_cache_consumers )
		{
			pthread_cond_signal( &self->a_cache_consumer_cond );
		}
	}
	// We need to cache the data for later
	else
	{
		// Make temp audio buffer bigger to get all the samples
		self->ta_buffer =
			mlt_pool_realloc( self->ta_buffer, self->ta_buffer_size + ( nb_samples * AUDIO_SAMPLE_SIZE * channels ) );

		// TODO: What now? Cleanup and terminate?
		assert( self->ta_buffer != NULL );

		memcpy( self->ta_buffer + self->ta_buffer_size,
				p_pcm_buffer, ( nb_samples * AUDIO_SAMPLE_SIZE * channels ) );
		self->ta_buffer_size += ( nb_samples * AUDIO_SAMPLE_SIZE * channels );
		mlt_pool_release( p_pcm_buffer );
	}
	pthread_mutex_unlock( &self->a_cache_mutex );
}

static void video_prerender_callback( void *data, uint8_t **p_buffer, size_t size )
{
	// Flag for termination purposes
	int mutex_locked = 0;

	producer_libvlc self = data;

	// If we're terminating, we need to abort render
	if ( self->terminating )
		goto terminate;

	// Aquire cache mutex
	pthread_mutex_lock( &self->v_cache_mutex );
	mutex_locked = 1;

	// Inform all threads we are waiting on cond
	self->v_cache_producers++;

	// Wait until there is free space in cache
	while( mlt_deque_count( self->v_cache ) == MAX_CACHE_SIZE )
	{
		pthread_cond_wait( &self->v_cache_producer_cond, &self->v_cache_mutex );

		// Terminate function could have woken us up
		if ( self->terminating )
			goto terminate;
	}

	// We're not waiting on cond anymore
	self->v_cache_producers--;

	// Allocate memory and proceed with rendering
	uint8_t *buffer = mlt_pool_alloc( size * sizeof( uint8_t ) );
	*p_buffer = buffer;

	return;

terminate:
	*p_buffer = NULL;
	// Release mutex for destruction
	if ( mutex_locked )
		pthread_mutex_unlock( &self->v_cache_mutex );
	return;
}

static void video_postrender_callback( void *data, uint8_t *buffer, int width, int height,
									   int bpp, size_t size, int64_t pts )
{
	size_t i;

	producer_libvlc self = data;
	mlt_properties properties = MLT_PRODUCER_PROPERTIES( self->parent );

	// For some reason width and height returned by smem
	// seems bogus (when using width * height * bpp / 8 as size
	// of returned buffer, accessing last bytes causes segfault),
	// so we're using width and height that we've inputted into
	// transcoder when constructing smem
	width = mlt_properties_get_int( properties, "width" );
	height = mlt_properties_get_int( properties, "height" );
	size_t buffer_size = width * height * bpp / 8;

	// Allocate space for cache data
	v_cache_item v_item = calloc( 1, sizeof( struct v_cache_item_s ) );

	// Exchange red with blue (for some reason libVLC has
	// has different representation of RGB24 than MLT)
	for (i = 0; i+2 < buffer_size; i += 3)
	{
		uint8_t temp = buffer[ i ];
		buffer[ i ] = buffer[ i + 2 ];
		buffer[ i + 2 ] = temp;
	}

	// Move stuff to cache
	v_item->buffer = buffer;
	v_item->size = size;
	v_item->bpp = bpp;
	v_item->width = width;
	v_item->height = height;

	// Push cache item into cache
	mlt_deque_push_back( self->v_cache, v_item );

	// Signal waiting consumers, if any
	if ( self->v_cache_consumers )
	{
		pthread_cond_signal( &self->v_cache_consumer_cond );
	}

	// Unlock cache access
	pthread_mutex_unlock( &self->v_cache_mutex );
}

static int producer_get_audio( mlt_frame frame, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
	// Get the producer
	producer_libvlc self = mlt_frame_pop_audio( frame );

	pthread_mutex_lock( &self->a_cache_mutex );
	self->a_cache_consumers++;
	// TODO: This logic will have to be rewritten when we implement seeking,
	// if we manipulate the video's position, we have no guarantee that
	// data in cache is actually relevant to us (same for video)
	while ( mlt_deque_count( self->a_cache ) == 0 ) {
		pthread_cond_wait( &self->a_cache_consumer_cond, &self->a_cache_mutex );
	}
	self->a_cache_consumers--;

	a_cache_item a_item = mlt_deque_pop_front( self->a_cache );
	*buffer = a_item->buffer;
	// We set libVLC's smem to use this format
	*format = mlt_audio_s16;
	*frequency = a_item->sample_rate;
	*channels = a_item->channels;
	*samples = a_item->samples;

	free( a_item );

	// Sets audio buffer destructor
	mlt_frame_set_audio( frame, *buffer, *format, *samples * *channels * AUDIO_SAMPLE_SIZE, mlt_pool_release );

	if ( self->a_cache_producers )
	{
		pthread_cond_signal( &self->a_cache_producer_cond );
	}
	pthread_mutex_unlock( &self->a_cache_mutex );
	return 0;
}

static int producer_get_image( mlt_frame frame, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable )
{
	// Get the producer
	producer_libvlc self = mlt_frame_pop_service( frame );

	mlt_producer producer = self->parent;

	// libVLC produces frames in this format
	*format = mlt_image_rgb24;

	// Lock the cache mutex
	pthread_mutex_lock( &self->v_cache_mutex );

	// Inform other threads that you're waiting for data
	self->v_cache_consumers++;

	// Wait for data
	while ( mlt_deque_count( self->v_cache ) == 0 )
	{
		pthread_cond_wait( &self->v_cache_consumer_cond, &self->v_cache_mutex );
	}

	// Not waiting for data anymore
	self->v_cache_consumers--;

	// Get data, rewrite it, and free cache item
	v_cache_item v_item = mlt_deque_pop_front( self->v_cache );
	*width = v_item->width;
	*height = v_item->height;
	*buffer = v_item->buffer;
	free( v_item );

	// Sets image buffer destructor
	mlt_frame_set_image( frame, *buffer, *width * *height * 3, mlt_pool_release );

	// Signal waiting producers, if any
	if ( self->v_cache_producers )
	{
		pthread_cond_signal( &self->v_cache_producer_cond );
	}

	// We're finished with cache
	pthread_mutex_unlock( &self->v_cache_mutex );

	return 0;
}

static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
	// Get handle to libVLC's producer
	producer_libvlc self = producer->child;

	// TODO: Sometimes this line causes segfault when using melt. Is it ours or melt's fault?
	// I assume in my code that MLT won't call producer_get_frame() after producer_close() but
	// maybe the assumption isn't correct?
	mlt_position current_position = mlt_producer_position( self );

	// Access the private data (producer is needed to get profile)
	mlt_service service = MLT_PRODUCER_SERVICE( producer );

	// Initialize frame with producer's profile and get its properties
	*frame = mlt_frame_init( service );
	mlt_properties frame_properties = mlt_frame_properties( *frame );

	// Push get_image and its argument to video stack of frame
	mlt_frame_push_service( *frame, self );
	mlt_frame_push_get_image( *frame, producer_get_image );

	mlt_frame_push_audio( *frame, self );
	mlt_frame_push_audio( *frame, producer_get_audio );

	// Prepare next frame
	mlt_producer_prepare_next( producer );

	return 0;
}

static void producer_close( mlt_producer parent )
{
	if ( parent != NULL ) {
		producer_libvlc self = parent->child;

		// Stop smem threads
		self->terminating = 1;
		libvlc_media_player_stop( self->media_player );

		// Release libVLC objects
		libvlc_media_player_release( self->media_player );
		libvlc_media_release( self->media );
		libvlc_release( self->libvlc_instance );

		// Clear video cache
		v_cache_item v_item;
		while ( v_item = mlt_deque_pop_front( self->v_cache ) ) {
			mlt_pool_release( v_item->buffer );
			free( v_item );
		}

		// Clear audio cache
		a_cache_item a_item;
		while ( a_item = mlt_deque_pop_front( self->a_cache ) ) {
			mlt_pool_release( a_item->buffer );
			free( a_item );
		}

		// Clear ta_buffer
		mlt_pool_release( self->ta_buffer );

		// Clear mutexes and conds
		pthread_mutex_destroy( &self->v_cache_mutex );
		pthread_cond_destroy( &self->v_cache_producer_cond );
		pthread_cond_destroy( &self->v_cache_consumer_cond );
		pthread_mutex_destroy( &self->a_cache_mutex );
		pthread_cond_destroy( &self->a_cache_producer_cond );
		pthread_cond_destroy( &self->a_cache_consumer_cond );

		// Free allocated memory for libvlc_producer
		free( self );

		// Call overriden destructor
		parent->close = NULL;
		mlt_producer_close( parent );
	}
}
