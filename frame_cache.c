/*
Consecutive frame cache.

This is written with libVLC producer in mind,
which produces consecutive raw audio/video samples,
then packs it into consecutive mlt_frames. The frames are then
put into this cache, so they could be available when some MLT
consumer requests them.

If we have a cache miss, we will usually need to seek libVLC,
purge cache, and start putting the frames into it again.

libVLC producer will produce non writable frames...
*/
#include <stdlib.h>

#include <framework/mlt_frame.h>
#include <framework/mlt_types.h>
#include <framework/mlt_pool.h>

#include "frame_cache.h"

struct frame_cache_s
{
	// Circular buffer of mlt_frames
	mlt_frame *frames;
	// Index, which points to first frame
	size_t start_pos;
	// How many frames are in cache currently
	size_t frames_total;
	// How many frames max in cache
	size_t size;
};

static ssize_t frame_cache_frame_index( frame_cache self, mlt_position position )
{
	if ( self->frames_total == 0 )
		return -1;

	mlt_frame first_frame = self->frames[ self->start_pos ];
	mlt_position first_frame_position = mlt_frame_original_position( first_frame );
	mlt_position last_frame_position = first_frame_position + self->frames_total - 1;
	if ( position >= first_frame_position && position <= last_frame_position )
	{
		int offset = position - first_frame_position;
		return ( self->start_pos + offset ) % self->size;
	}
	return -1;
}

frame_cache frame_cache_init( size_t size_max )
{
	// Empty frame cache is useless
	if ( size_max == 0 )
		return NULL;

	frame_cache cache = calloc( 1, sizeof( struct frame_cache_s ) );
	if ( cache != NULL )
	{
		cache->frames = mlt_pool_alloc( size_max * sizeof( mlt_frame ) );
		if ( cache->frames == NULL )
		{
			free( cache );
			return NULL;
		}
		cache->start_pos = 0;
		cache->frames_total = 0;
		cache->size = size_max;
	}
	return cache;
}

mlt_frame frame_cache_get_frame( frame_cache self, mlt_position position )
{
	// Return NULL if cache miss/fail
	mlt_frame frame = NULL;

	if ( self == NULL )
		return frame;

	if ( self->frames_total > 0 )
	{
		ssize_t index = frame_cache_frame_index( self, position );
		if ( index != -1 )
			frame = self->frames[ index ];
	}

	// We managed to get frame from cache, so retain it to share ownership with the client
	if ( frame != NULL )
	{
		mlt_properties_inc_ref( MLT_FRAME_PROPERTIES( frame ) );
	}

	return frame;
}

int frame_cache_put_frame( frame_cache self, mlt_frame frame )
{
	if ( self == NULL )
		return 1;

	mlt_position frame_position = mlt_frame_original_position( frame );
	// We actually need to insert frame into cache
	if ( frame_cache_frame_index( self, frame_position ) == -1 )
	{
		if ( self->frames_total > 0 )
		{
			mlt_frame first_frame = self->frames[ self->start_pos ];
			mlt_position first_frame_position = mlt_frame_original_position( first_frame );
			mlt_position last_frame_position = first_frame_position + self->frames_total - 1;
			// We're trying to insert next frame (in sequence), so no need to delete previous ones
			if ( frame_position == last_frame_position + 1 )
			{
				// Append new frame at the end of circular buffer
				if ( self->frames_total < self->size )
				{
					size_t index = ( self->start_pos + self->frames_total ) % self->size;
					self->frames[ index ] = frame;
					self->frames_total += 1;
				}
				// We need to throw out the earliest frame
				else
				{
					self->frames[ self->start_pos ] = frame;
					self->start_pos = ( self->start_pos + 1 ) % self->size;
				}
			}
			// We're inserting frame that is not in cache, and is not next in sequence
			else
			{
				frame_cache_purge( self );
				self->frames[ self->start_pos ] = frame;
				self->frames_total += 1;
			}
		}
		else
		{
			self->frames[ self->start_pos ] = frame;
			self->frames_total += 1;
		}
	}
	return 0;
}

mlt_position frame_cache_earliest_frame_position( frame_cache self )
{
	if ( self == NULL )
		return FRAME_CACHE_INVALID_POSITION;

	if ( self->frames_total == 0 )
		return FRAME_CACHE_INVALID_POSITION;

	mlt_frame earliest_frame = self->frames[ self->start_pos ];

	return mlt_frame_original_position( earliest_frame );
}

mlt_position frame_cache_latest_frame_position( frame_cache self )
{
	if ( self == NULL )
		return FRAME_CACHE_INVALID_POSITION;

	if ( self->frames_total == 0 )
		return FRAME_CACHE_INVALID_POSITION;

	mlt_frame latest_frame =
		self->frames[ ( self->start_pos + self->frames_total - 1 ) % self->size ];

	return mlt_frame_original_position( latest_frame );
}

void frame_cache_purge( frame_cache self )
{
	if ( self == NULL )
		return;

	size_t iter;
	for ( iter = 0; iter < self->frames_total; iter++ )
	{
		size_t current_index = ( self->start_pos + iter ) % self->size;
		mlt_frame_close( self->frames[ current_index ] );
	}

	self->frames_total = 0;
}

void frame_cache_close( frame_cache self )
{
	if ( self == NULL )
		return;

	frame_cache_purge( self );
	mlt_pool_release( self->frames );
	free( self );
}
