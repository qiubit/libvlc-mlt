#include <stdlib.h>

#include <framework/mlt_service.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_profile.h>
#include <framework/mlt_log.h>
#include <framework/mlt_pool.h>
#include <framework/mlt_types.h>

#include "buffer_queue.h"

struct buffer_wrapper_s
{
	uint8_t *buffer;
	size_t buffer_pos;
	size_t buffer_size;
};

struct buffer_queue_s
{
	// Owner of buffer_queue (that's where metadata is fetched from during initialization)
	mlt_service owner;

	// Total number of audio samples contained in audio_contents
	unsigned int nb_audio_samples;
	// Audio format used for storing audio
	mlt_audio_format afmt;
	// Number of channels in audio stored
	int channels;
	// Sample rate of audio stored
	int samplerate;
	// Audio buffer data
	mlt_deque audio_contents;

	// Image format used for storing image
	mlt_image_format vfmt;
	// Video buffer data
	mlt_deque video_contents;
};

typedef struct buffer_wrapper_s *buffer_wrapper;

buffer_queue buffer_queue_init( mlt_service owner, mlt_image_format vfmt, mlt_audio_format afmt, int channels, int samplerate )
{
	if ( owner == NULL )
		return NULL;

	buffer_queue queue = calloc( 1, sizeof( struct buffer_queue_s ) );
	if ( queue != NULL )
	{
		queue->owner = owner;

		queue->nb_audio_samples = 0;
		queue->afmt = afmt;
		queue->channels = channels;
		queue->samplerate = samplerate;
		queue->audio_contents = mlt_deque_init( );
		if ( queue->audio_contents == NULL ) goto cleanup;

		queue->vfmt = vfmt;
		queue->video_contents = mlt_deque_init( );
		if ( queue->video_contents == NULL ) goto cleanup;
	}
	return queue;

cleanup:
	if ( queue && queue->audio_contents ) mlt_deque_close( queue->audio_contents );
	if ( queue && queue->video_contents ) mlt_deque_close( queue->video_contents );
	free( queue );
	return NULL;
}

static int buffer_queue_insert_buffer( buffer_queue self, uint8_t *audio_buffer, size_t size, int is_audio )
{
	buffer_wrapper bw = calloc( 1, sizeof( struct buffer_wrapper_s ) );
	if ( bw == NULL )
		return 1;

	bw->buffer = audio_buffer;
	bw->buffer_pos = 0;
	bw->buffer_size = size;

	mlt_deque deque_to_push;
	if ( is_audio )
		deque_to_push = self->audio_contents;
	else
		deque_to_push = self->video_contents;

	if ( mlt_deque_push_back( deque_to_push, bw ) )
	{
		free( bw );
		return 1;
	}

	// If we just pushed audio buffer, we need to update samples count
	if ( is_audio )
	{
		unsigned int samples_in_buffer =
			size / mlt_audio_format_size( self->afmt, 1, self->channels );
		if ( size % mlt_audio_format_size( self->afmt, 1, self->channels ) != 0 )
			mlt_log( self->owner, MLT_LOG_WARNING, "%s\n", "buffer_queue_insert_buffer: Invalid audio buffer size detected\n" );
		self->nb_audio_samples += samples_in_buffer;
	}

	return 0;
}

int buffer_queue_insert_audio_buffer( buffer_queue self, uint8_t *audio_buffer, size_t size )
{
	return buffer_queue_insert_buffer( self, audio_buffer, size, 1 );
}

int buffer_queue_insert_video_buffer( buffer_queue self, uint8_t *video_buffer, size_t size )
{
	return buffer_queue_insert_buffer( self, video_buffer, size, 0 );
}

mlt_frame buffer_queue_pack_frame( buffer_queue self, mlt_position position )
{
	mlt_frame frame = NULL;
	mlt_profile profile = mlt_service_profile( self->owner );


	// Firstly, if we don't have any image buffers, we won't make a frame
	if ( mlt_deque_count( self->video_contents ) == 0 )
		return NULL;

	// Secondly, if we don't have enough audio samples, we won't make a frame too
	int needed_samples = mlt_sample_calculator( mlt_profile_fps( profile ), self->samplerate, position );
	if ( needed_samples > self->nb_audio_samples )
	{
		return NULL;
	}

	// We have all necessary data to pack the frame, so it's time to do it
	frame = mlt_frame_init( self->owner );
	if ( frame == NULL )
		return NULL;
	mlt_properties frame_properties = mlt_frame_properties( frame );

	uint8_t *audio_buffer = NULL;
	uint8_t *video_buffer = NULL;
	size_t audio_buffer_size;
	size_t video_buffer_size;

	audio_buffer_size = mlt_audio_format_size( self->afmt, needed_samples, self->channels );
	audio_buffer = mlt_pool_alloc( audio_buffer_size );
	if ( audio_buffer == NULL )
		return NULL;

	// First we fill the audio buffers TODO
	size_t audio_buffer_iter = 0;
	while ( audio_buffer_iter < audio_buffer_size )
	{
		buffer_wrapper abw = mlt_deque_pop_front( self->audio_contents );
		while ( abw->buffer_pos != abw->buffer_size && audio_buffer_iter < audio_buffer_size )
		{
			audio_buffer[ audio_buffer_iter ] = abw->buffer[ abw->buffer_pos ];
			audio_buffer_iter++;
			abw->buffer_pos++;
		}
		// We've got some samples left in abw, but we finished packing frame
		if ( abw->buffer_pos != abw->buffer_size )
		{
			mlt_deque_push_front( self->audio_contents, abw );
		}
		// No samples left in abw
		else
		{
			mlt_pool_release( abw->buffer );
			free( abw );
		}
	}
	self->nb_audio_samples -= needed_samples;

	// Now we fill the video buffer
	buffer_wrapper vbw = mlt_deque_pop_front( self->video_contents );
	video_buffer = vbw->buffer;
	video_buffer_size = vbw->buffer_size;
	free( vbw );

	// Now it's time to bind the buffers to mlt_frame
	// TODO: Is this approach compatible with image/audio frame stacks?
	mlt_frame_set_audio( frame, audio_buffer, self->afmt, audio_buffer_size, ( mlt_destructor )mlt_pool_release );
	mlt_properties_set_int( frame_properties, "audio_frequency", self->samplerate );
	mlt_properties_set_int( frame_properties, "audio_channels", self->channels );
	mlt_properties_set_int( frame_properties, "audio_samples", needed_samples );

	mlt_frame_set_image( frame, video_buffer, video_buffer_size, ( mlt_destructor )mlt_pool_release );
	mlt_properties_set_int( frame_properties, "format", self->vfmt );
	mlt_properties_set_int( frame_properties, "width", profile->width );
	mlt_properties_set_int( frame_properties, "height", profile->height );

	mlt_frame_set_position( frame, position );

	return frame;
}

void buffer_queue_purge( buffer_queue self )
{
	if ( self == NULL )
		return;

	buffer_wrapper bw;
	while ( bw = mlt_deque_pop_front( self->audio_contents ) )
	{
		mlt_pool_release( bw->buffer );
		free( bw );
	}
	while ( bw = mlt_deque_pop_front( self->video_contents ) )
	{
		mlt_pool_release( bw->buffer );
		free( bw );
	}

	self->nb_audio_samples = 0;
}

void buffer_queue_close( buffer_queue self )
{
	if ( self == NULL )
		return;

	buffer_queue_purge( self );
	mlt_deque_close( self->audio_contents );
	mlt_deque_close( self->video_contents );
	free( self );
}