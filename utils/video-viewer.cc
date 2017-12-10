// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Quick hack based on ffmpeg
// tutorial http://dranger.com/ffmpeg/tutorial01.html
// in turn based on a tutorial by
// Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
//

// Ancient AV versions forgot to set this.
#define __STDC_CONSTANT_MACROS

// libav: "U NO extern C in header ?"
extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "led-matrix.h"
#include "content-streamer.h"

using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::StreamWriter;
using rgb_matrix::StreamIO;
typedef int64_t tmillis_t;

volatile bool interrupt_received = false;
static void InterruptHandler(int) {
  interrupt_received = true;
}

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#  define av_frame_alloc avcodec_alloc_frame
#  define av_frame_free avcodec_free_frame
#endif

struct LedPixel {
  uint8_t r, g, b;
};
void CopyFrame(AVFrame *pFrame, FrameCanvas *canvas) {
  // Write pixel data
  const int height = canvas->height();
  const int width = canvas->width();
  for(int y = 0; y < height; ++y) {
    LedPixel *pix = (LedPixel*) (pFrame->data[0] + y*pFrame->linesize[0]);
    for(int x = 0; x < width; ++x, ++pix) {
      canvas->SetPixel(x, y, pix->r, pix->g, pix->b);
    }
  }
}

static tmillis_t GetTimeInMillis() {
  struct timeval tp;
  gettimeofday(&tp, NULL);
  return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s [options] <video>\n", progname);
  fprintf(stderr, "Options:\n"
          "\t-O<streamfile>     : Output to stream-file instead of matrix (don't need to be root).\n"
          "\t-L                 : Large display, in which each chain is 'folded down'\n"
          "\t                     in the middle in an U-arrangement to get more vertical space.\n"
          "\t-R<angle>          : Rotate output; steps of 90 degrees\n"
          "\t-r<repeat-secs>    : Repeat for at least n seconds\n"
          "\t-v                 : verbose.\n");

  fprintf(stderr, "\nGeneral LED matrix options:\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  bool large_display = false;  // 64x64 made out of 4 in sequence.
  int angle = -361;
  bool verbose = false;
  int repeatSeconds = 0;
  const char *stream_output = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "vO:R:r:L")) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    case 'O':
      stream_output = strdup(optarg);
      break;
    case 'L':
      if (matrix_options.chain_length == 1) {
        // If this is still default, force the 64x64 arrangement.
        matrix_options.chain_length = 4;
      }
      large_display = true;
      break;
    case 'R':
      angle = atoi(optarg);
      break;
    case 'r':
      repeatSeconds = atoi(optarg);
      break;

    default:
      return usage(argv[0]);
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "Expected video filename.\n");
    return usage(argv[0]);
  }

  runtime_opt.do_gpio_init = (stream_output == NULL);
  RGBMatrix *matrix = CreateMatrixFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) return 1;

  if (large_display)
    // Mapping the coordinates of a 32x128 display mapped to a square of 64x64,
    // or any other U-shape.
    matrix->ApplyStaticTransformer(rgb_matrix::UArrangementTransformer(
                                     matrix_options.parallel));

  if (angle >= -360)
    matrix->ApplyStaticTransformer(rgb_matrix::RotateTransformer(angle));

  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();
  StreamIO *stream_io = NULL;
  StreamWriter *stream_writer = NULL;
  if (stream_output) {
    int fd = open(stream_output, O_CREAT|O_WRONLY, 0644);
    if (fd < 0) {
      perror("Couldn't open output stream");
      return 1;
    }
    stream_io = new rgb_matrix::FileStreamIO(fd);
    stream_writer = new StreamWriter(stream_io);
  }

  // Initalizing these to NULL prevents segfaults!
  AVFormatContext   *pFormatCtx = NULL;
  int               i, videoStream;
  AVCodecContext    *pCodecCtxOrig = NULL;
  AVCodecContext    *pCodecCtx = NULL;
  AVCodec           *pCodec = NULL;
  AVFrame           *pFrame = NULL;
  AVFrame           *pFrameRGB = NULL;
  AVPacket          packet;
  int               frameFinished;
  int               numBytes;
  uint8_t           *buffer = NULL;
  struct SwsContext *sws_ctx = NULL;

  // Register all formats and codecs
  av_register_all();
  avformat_network_init();

  for (int imgarg = optind; imgarg < argc && !interrupt_received; ++imgarg) {
    const char *movie_file = argv[imgarg];

    // Open video file
    if(avformat_open_input(&pFormatCtx, movie_file, NULL, NULL)!=0) {
      fprintf(stderr, "Couldn't open file %s\n", movie_file);
      continue;
    }
    fprintf(stderr, "Playing %s\n", movie_file);

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
      return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    if (verbose) {
      av_dump_format(pFormatCtx, 0, movie_file, 0);
    }

    long frame_count = 0;
    // Find the first video stream
    videoStream=-1;
    for (i=0; i < (int)pFormatCtx->nb_streams; ++i) {
      if (pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO) {
        videoStream=i;
        break;
      }
    }
    if (videoStream == -1)
      return -1; // Didn't find a video stream

    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;
    double fps = av_q2d(pFormatCtx->streams[videoStream]->avg_frame_rate);
    if (fps < 0) {
      fps = 1.0 / av_q2d(pFormatCtx->streams[videoStream]->codec->time_base);
    }
    if (verbose) fprintf(stderr, "FPS: %f\n", fps);

    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if (pCodec==NULL) {
      fprintf(stderr, "Unsupported codec!\n");
      continue;
    }
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
      fprintf(stderr, "Couldn't copy codec context");
      continue;
    }

    // Open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL)<0)
      continue;

    // Allocate video frame
    pFrame=av_frame_alloc();

    // Allocate an AVFrame structure
    pFrameRGB=av_frame_alloc();
    if (pFrameRGB==NULL)
      continue;

    // Determine required buffer size and allocate buffer
    numBytes=avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
                                pCodecCtx->height);
    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    avpicture_fill((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24,
                   pCodecCtx->width, pCodecCtx->height);

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
                             pCodecCtx->height,
                             pCodecCtx->pix_fmt,
                             matrix->width(), matrix->height(),
                             AV_PIX_FMT_RGB24,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL
                             );
    if (sws_ctx == 0) {
      fprintf(stderr, "Trouble doing scaling to %dx%d :(\n",
              matrix->width(), matrix->height());
      return 1;
    }

    signal(SIGTERM, InterruptHandler);
    signal(SIGINT, InterruptHandler);

    const int frame_wait_micros = 1e6 / fps;

    const tmillis_t startPlay = GetTimeInMillis(); //mark time
    int repeatedcount = 0;
    do {
      frame_count = 0;
      while (!interrupt_received && av_read_frame(pFormatCtx, &packet) >= 0) {
        // Is this a packet from the video stream?
        if (packet.stream_index==videoStream) {
          // Decode video frame
          avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

          // Did we get a video frame?
          if (frameFinished) {
            // Convert the image from its native format to RGB
            sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                      pFrame->linesize, 0, pCodecCtx->height,
                      pFrameRGB->data, pFrameRGB->linesize);

            CopyFrame(pFrameRGB, offscreen_canvas);
            frame_count++;
            if (stream_writer) {
              stream_writer->Stream(*offscreen_canvas, frame_wait_micros);
            } else {
              offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas);
            }
          }
          if (!stream_writer) usleep(frame_wait_micros);
        }

        // Free the packet that was allocated by av_read_frame
        av_free_packet(&packet);
      }
      if ((GetTimeInMillis() - startPlay) < (repeatSeconds * 1000) && !av_seek_frame(pFormatCtx, -1, 1, AVSEEK_FLAG_FRAME)) {
        if (repeatedcount)
          fprintf(stderr, "finished loop %d (%ld frames) after %0.1fs\n", repeatedcount, frame_count, (GetTimeInMillis() - startPlay) / 1000.);
        repeatedcount++;
        continue;
      } else break;
    } while (!interrupt_received);

    if (interrupt_received) {
      // Feedback for Ctrl-C, but most importantly, force a newline
      // at the output, so that commandline-shell editing is not messed up.
      fprintf(stderr, "Got interrupt. Exiting\n");
      break;
    }

    // Free the RGB image
    av_free(buffer);
    av_frame_free(&pFrameRGB);

    // Free the YUV frame
    av_frame_free(&pFrame);

    // Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    delete stream_writer;
    delete stream_io;
    fprintf(stderr, "Finished playing %s - %ld frames for %0.1fs\n", movie_file, frame_count, (GetTimeInMillis() - startPlay) / 1000.);
  }
  return 0;
}
