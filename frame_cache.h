#ifndef FRAME_CACHE_H
#define FRAME_CACHE_H

#include <framework/mlt_frame.h>

#define FRAME_CACHE_INVALID_POSITION (-1)

typedef struct frame_cache_s *frame_cache;

extern frame_cache frame_cache_init( size_t size_max );
extern mlt_frame frame_cache_get_frame( frame_cache self, mlt_position position );
extern int frame_cache_put_frame( frame_cache self, mlt_frame frame );
extern mlt_position frame_cache_earliest_frame_position( frame_cache self );
extern mlt_position frame_cache_latest_frame_position( frame_cache self );
extern void frame_cache_purge( frame_cache self );
extern void frame_cache_close( frame_cache self );

#endif