
#include "zm_ffmpeg_input.h"
#include "zm_logger.h"
#include "zm_ffmpeg.h"

FFmpeg_Output::FFmpeg_Output() {
  input_format_context = NULL;
  video_stream_id = -1;
  audio_stream_id = -1;
  av_register_all();
  avcodec_register_all();

}
FFmpeg_Output::~FFmpeg_Output() {
}

int FFmpeg_Output::Open( const char *filepath ) {

  int error;

  /** Open the input file to read from it. */
  if ( (error = avformat_open_input( &input_format_context, filepath, NULL, NULL)) < 0 ) {

    Error("Could not open input file '%s' (error '%s')\n",
        filepath, av_make_error_string(error).c_str() );
    input_format_context = NULL;
    return error;
  } 

  /** Get information on the input file (number of streams etc.). */
  if ( (error = avformat_find_stream_info(input_format_context, NULL)) < 0 ) {
    Error( "Could not open find stream info (error '%s')\n",
        av_make_error_string(error).c_str() );
    avformat_close_input(&input_format_context);
    return error;
  }

  for ( unsigned int i = 0; i < input_format_context->nb_streams; i += 1 ) {
    if ( is_video_stream( input_format_context->streams[i] ) ) {
      zm_dump_stream_format(input_format_context, i, 0, 0);
      if ( video_stream_id == -1 ) {
        video_stream_id = i;
        // if we break, then we won't find the audio stream
      } else {
        Warning( "Have another video stream." );
      }
    } else if ( is_audio_stream( input_format_context->streams[i] ) ) {
      if ( audio_stream_id == -1 ) {
        audio_stream_id = i;
      } else {
        Warning( "Have another audio stream." );
      }
    }

    streams[i].frame_count = 0;
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    streams[i].context = avcodec_alloc_context3( NULL );
    avcodec_parameters_to_context( streams[i].context, input_format_context->streams[i]->codecpar );
#else
    streams[i].context = input_format_context->streams[i]->codec;
#endif

    if ( !(streams[i].codec = avcodec_find_decoder(streams[i].context->codec_id)) ) {
      Error( "Could not find input codec\n");
      avformat_close_input(&input_format_context);
      return AVERROR_EXIT;
    } else {
      Debug(1, "Using codec (%s) for stream %d", streams[i].codec->name, i );
    }

    if ((error = avcodec_open2( streams[i].context, streams[i].codec, NULL)) < 0) {
      Error( "Could not open input codec (error '%s')\n",
          av_make_error_string(error).c_str() );
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
      avcodec_free_context( &streams[i].context );
#endif
      avformat_close_input(&input_format_context);
      return error;
    }
  } // end foreach stream

  if ( video_stream_id == -1 )
    Error( "Unable to locate video stream in %s", filepath );
  if ( audio_stream_id == -1 )
    Debug( 3, "Unable to locate audio stream in %s", filepath );

  return 0;
} // end int FFmpeg_Output::Open( const char * filepath )

AVFrame *FFmpeg_Output::get_frame( int stream_id ) {
  Debug(1, "Getting frame from stream %d", stream_id );

  int frameComplete = false;
  AVPacket packet;
  av_init_packet( &packet );
  AVFrame *frame = zm_av_frame_alloc();
  char errbuf[AV_ERROR_MAX_STRING_SIZE];

  while ( !frameComplete ) {
    int ret = av_read_frame( input_format_context, &packet );
    if ( ret < 0 ) {
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      if (
          // Check if EOF.
          (ret == AVERROR_EOF || (input_format_context->pb && input_format_context->pb->eof_reached)) ||
          // Check for Connection failure.
          (ret == -110)
         ) {
        Info( "av_read_frame returned %s.", errbuf );
        return NULL;
      }
      Error( "Unable to read packet from stream %d: error %d \"%s\".", packet.stream_index, ret, errbuf );
      return NULL;
    }

    if ( (stream_id < 0 ) || ( packet.stream_index == stream_id ) ) {
      Debug(1,"Packet is for our stream (%d)", packet.stream_index );

      AVCodecContext *context = streams[packet.stream_index].context;

#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    ret = avcodec_send_packet( context, &packet );
    if ( ret < 0 ) {
      av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
      Error( "Unable to send packet at frame %d: %s, continuing", streams[packet.stream_index].frame_count, errbuf );
      zm_av_packet_unref( &packet );
      continue;
    } else {
      Debug(1, "Success getting a packet");
    }

#if HAVE_AVUTIL_HWCONTEXT_H
    if ( hwaccel ) {
      ret = avcodec_receive_frame( context, hwFrame );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to receive frame %d: %s, continuing", streams[packet.stream_index].frame_count, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
      ret = av_hwframe_transfer_data(frame, hwFrame, 0);
      if (ret < 0) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to transfer frame at frame %d: %s, continuing", streams[packet.stream_index].frame_count, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
    } else {
#endif
      Debug(1,"Getting a frame?");
      ret = avcodec_receive_frame( context, frame );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to send packet at frame %d: %s, continuing", streams[packet.stream_index].frame_count, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }

#if HAVE_AVUTIL_HWCONTEXT_H
    }
#endif

    frameComplete = 1;
# else
    ret = zm_avcodec_decode_video( streams[packet.stream_index].context, frame, &frameComplete, &packet );
    if ( ret < 0 ) {
      av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
      Error( "Unable to decode frame at frame %d: %s, continuing", streams[packet.stream_index].frame_count, errbuf );
      zm_av_packet_unref( &packet );
      continue;
    }
#endif
  } // end if it's the right stream

      zm_av_packet_unref( &packet );

  } // end while ! frameComplete
  return frame;

} //  end AVFrame *FFmpeg_Output::get_frame
