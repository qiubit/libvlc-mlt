#include <framework/mlt_types.h>

typedef struct buffer_queue_s *buffer_queue;

extern buffer_queue buffer_queue_init( mlt_service owner, mlt_image_format vfmt, mlt_audio_format afmt, int channels, int samplerate );
extern int buffer_queue_insert_audio_buffer( buffer_queue self, uint8_t *audio_buffer, size_t size );
extern int buffer_queue_insert_video_buffer( buffer_queue self, uint8_t *video_buffer, size_t size );
extern mlt_frame buffer_queue_pack_frame( buffer_queue self, mlt_position position );
extern void buffer_queue_purge( buffer_queue self );
extern void buffer_queue_close( buffer_queue self );
