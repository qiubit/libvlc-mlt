/*
Once user "seeks" our MLT producer, we need
to seek smem as well. This introduces the
need of being able to detect, which data
produced by smem is "before-seek", and which
is "after-seek". This demo presents a solution
I use for that purpose.
*/

#include <vlc_common.h>
#include <vlc/vlc.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#define FRAMES_FOR_PTS_DIFF 5
#define NO_TIMESTAMP -1

int collecting_pts_diff = 0;
int waiting_for_pts_diff = 0;

int64_t audio_pts[ FRAMES_FOR_PTS_DIFF ];
int64_t video_pts[ FRAMES_FOR_PTS_DIFF ];
int audio_pts_collected = 0;
int video_pts_collected = 0;
int64_t average_audio_pts_diff;
int64_t average_video_pts_diff;
pthread_mutex_t pts_diff_mutex;
pthread_cond_t pts_diff_cond;

int64_t abs64( int64_t num )
{
	if ( num < 0 )
		return -num;
	return num;
}

int64_t previous_audio_timestamp = NO_TIMESTAMP;
int64_t previous_video_timestamp = NO_TIMESTAMP;

int smem_init( libvlc_media_t* );

void log_swallower( void*, int, libvlc_log_t const*, char const*, va_list args);

void smem_allocator( void*, uint8_t**, size_t );
void smem_video_cb( void*, uint8_t*, int, int, int, size_t, int64_t );
void smem_audio_cb( void*, uint8_t*, unsigned, unsigned,
  unsigned, unsigned, size_t, int64_t );
void smem_pts_diff_collector( libvlc_media_player_t * );

int
main( int argc, char** argv )
{
    libvlc_instance_t*     p_vlc    = NULL;
    libvlc_media_player_t* p_player = NULL;
    libvlc_media_t*        p_media  = NULL;

    int ret = 0;

    if( argc < 2 )
    {
        fprintf( stderr, "USAGE: %s <path-to-file> [VLC-args...]", argv[0] );
        goto error;
    }

    p_vlc = libvlc_new( argc - 2, (char const**)argv + 2 );

    libvlc_log_set( p_vlc, &log_swallower, NULL );

    if( unlikely( !p_vlc ) )
        goto error;

    p_media = libvlc_media_new_path( p_vlc, argv[1] );

    if( unlikely( !p_media ) )
        goto error;

    if( smem_init( p_media ) )
        goto error;

    p_player = libvlc_media_player_new_from_media( p_media );

    if( unlikely( !p_player ) )
        goto error;

    smem_pts_diff_collector( p_player );

    libvlc_media_player_play( p_player );

    sleep( 1 );

    printf( "--------- SET TIME ----------\n" );

    libvlc_media_player_set_time( p_player, 0 );

    sleep( 1 );

    libvlc_media_player_stop( p_player );

done:
    if( p_player ) libvlc_media_player_release( p_player );
    if( p_media ) libvlc_media_release( p_media );
    if( p_vlc ) libvlc_release( p_vlc );

    return ret;

error:
    ret = -1;
    goto done;
}

void log_swallower( void* data, int level, const libvlc_log_t* ctx,
  const char *fmt, va_list args )
{
    VLC_UNUSED( data );
    VLC_UNUSED( level );
    VLC_UNUSED( ctx );
    VLC_UNUSED( fmt );
    VLC_UNUSED( args );
}

int
smem_init( libvlc_media_t* p_media )
{
    char buf[ 512 ];

    if( snprintf( buf, sizeof( buf ),
        ":sout=#transcode{acodec=s16l}:smem{"
        "audio-prerender-callback=%"  PRIdPTR ","
        "video-prerender-callback=%"  PRIdPTR ","
        "audio-postrender-callback=%" PRIdPTR ","
        "video-postrender-callback=%" PRIdPTR "}",
        (intptr_t)&smem_allocator,
        (intptr_t)&smem_allocator,
        (intptr_t)&smem_audio_cb,
        (intptr_t)&smem_video_cb ) > (int)sizeof( buf ) )
    {
        return VLC_EGENERIC;
    }

    libvlc_media_add_option( p_media, buf );

    return VLC_SUCCESS;
}


void
smem_allocator( void* data, uint8_t** p_dst, size_t size )
{
    *p_dst = malloc( size );

    VLC_UNUSED( data );
}

void
smem_video_cb( void* data, uint8_t* p_buffer, int width, int height, int bpp,
  size_t size, int64_t i_pts )
{
	if ( collecting_pts_diff )
	{
		pthread_mutex_lock( &pts_diff_mutex );
		if ( video_pts_collected < FRAMES_FOR_PTS_DIFF )
		{
			video_pts[ video_pts_collected ] = i_pts;
			video_pts_collected++;
			if ( video_pts_collected == FRAMES_FOR_PTS_DIFF && audio_pts_collected == FRAMES_FOR_PTS_DIFF && waiting_for_pts_diff )
			{
				waiting_for_pts_diff = 0;
				pthread_cond_signal( &pts_diff_cond );
			}
		}
		pthread_mutex_unlock( &pts_diff_mutex );
	}
	else
	{
		printf( "got video pts: %" PRIu64 "\n", i_pts );
		if ( previous_video_timestamp != NO_TIMESTAMP )
		{
			int64_t pts_diff = i_pts - previous_video_timestamp;
			if ( abs64( pts_diff ) > average_video_pts_diff + average_video_pts_diff / 2 )
			{
				printf( "video seek detected\n" );
			}
		}
		previous_video_timestamp = i_pts;
	}

    VLC_UNUSED( data );
    VLC_UNUSED( width );
    VLC_UNUSED( height );
    VLC_UNUSED( bpp );
    VLC_UNUSED( size );

    free( p_buffer );
}

void
smem_audio_cb( void* data, uint8_t* p_buffer, unsigned channels, unsigned rate,
  unsigned nb_samples, unsigned bits_per_sample, size_t size, int64_t i_pts )
{
    if ( collecting_pts_diff )
	{
		pthread_mutex_lock( &pts_diff_mutex );
		if ( audio_pts_collected < FRAMES_FOR_PTS_DIFF )
		{
			audio_pts[ audio_pts_collected ] = i_pts;
			audio_pts_collected++;
			if ( video_pts_collected == FRAMES_FOR_PTS_DIFF && audio_pts_collected == FRAMES_FOR_PTS_DIFF && waiting_for_pts_diff )
			{
				waiting_for_pts_diff = 0;
				pthread_cond_signal( &pts_diff_cond );
			}
		}
		pthread_mutex_unlock( &pts_diff_mutex );
	}
	else
	{
		printf( "got audio pts: %" PRIu64 "\n", i_pts );
		if ( previous_audio_timestamp != NO_TIMESTAMP )
		{
			int64_t pts_diff = i_pts - previous_audio_timestamp;
			if ( abs64( pts_diff ) > average_audio_pts_diff + average_audio_pts_diff / 2 )
			{
				printf( "audio seek detected\n" );
			}
		}
		previous_audio_timestamp = i_pts;
	}

    VLC_UNUSED( data );
    VLC_UNUSED( p_buffer );
    VLC_UNUSED( channels );
    VLC_UNUSED( rate );
    VLC_UNUSED( nb_samples );
    VLC_UNUSED( bits_per_sample );
    VLC_UNUSED( size );

    free( p_buffer );
}

void
smem_pts_diff_collector( libvlc_media_player_t* player )
{
	int i;

	pthread_mutex_init( &pts_diff_mutex, NULL );
	pthread_cond_init( &pts_diff_cond, NULL );
	collecting_pts_diff = 1;
	waiting_for_pts_diff = 1;
	libvlc_media_player_play( player );
	pthread_mutex_lock( &pts_diff_mutex );
	while ( !( audio_pts_collected == FRAMES_FOR_PTS_DIFF && video_pts_collected == FRAMES_FOR_PTS_DIFF ) )
	{
		pthread_cond_wait( &pts_diff_cond, &pts_diff_mutex );
	}
	waiting_for_pts_diff = 0;
	pthread_mutex_unlock( &pts_diff_mutex );
	libvlc_media_player_stop( player );
	collecting_pts_diff = 0;
	for ( i = 1; i < FRAMES_FOR_PTS_DIFF; i++ )
	{
		average_audio_pts_diff += audio_pts[ i ] - audio_pts[ i - 1 ];
		average_video_pts_diff += video_pts[ i ] - video_pts[ i - 1 ];
	}
	average_audio_pts_diff /= FRAMES_FOR_PTS_DIFF - 1;
	average_video_pts_diff /= FRAMES_FOR_PTS_DIFF - 1;

	printf( "average_audio_pts_diff: %" PRId64 "\n", average_audio_pts_diff );
	printf( "average_video_pts_diff: %" PRId64 "\n", average_video_pts_diff );
}
