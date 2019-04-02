#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>

extern "C" {
    #include <libavutil/timestamp.h>
    #include <libavcodec/avcodec.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersrc.h>
    #include <libavfilter/buffersink.h>
    #include <libavformat/avformat.h>
    #include <libavformat/avio.h>
    #include <libswscale/swscale.h>
    #include <libavdevice/avdevice.h>
}

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

} FilteringContext;


struct Transcoder{
    Transcoder(){
        filter_ctx = (FilteringContext*) calloc(1,sizeof(FilteringContext)) ;
        stream_ctx = (StreamContext*) calloc(1, sizeof(StreamContext)) ;
    }
    ~Transcoder(){
        avformat_free_context(input_ctx);
        avcodec_close(stream_ctx->dec_ctx);
        avcodec_free_context(&stream_ctx->dec_ctx);

        avformat_free_context(output_ctx);
        avcodec_close(stream_ctx->enc_ctx);
        avcodec_free_context(&stream_ctx->enc_ctx);

        avfilter_free(filter_ctx->buffersrc_ctx);
        avfilter_free(filter_ctx->buffersink_ctx);
        avfilter_graph_free(&filter_ctx->filter_graph);

    }
    AVFormatContext *input_ctx = nullptr;
    AVFormatContext *output_ctx = nullptr;
    FilteringContext *filter_ctx = nullptr;
    StreamContext *stream_ctx = nullptr;

    int video_index = -1;

    int open_stream(const std::string & stream_uri);
    int open_output(const std::string & filename);
    int init_encoder();
    int init_decoder();
    int init_filtergraph(const std::string &filterstring);
    void loop(int count);

};

int Transcoder::open_stream(const std::string &stream_uri) {

    input_ctx = avformat_alloc_context();
    input_ctx->use_wallclock_as_timestamps = 1;

    //open video stream
    if (avformat_open_input(&input_ctx, stream_uri.c_str(), nullptr, nullptr) != 0) {
        return EXIT_FAILURE;
    }

    if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
        return EXIT_FAILURE;
    }

    //I know that for this task video stream always be at 0 index, but..
    video_index = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (video_index < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return video_index;
    }

    av_dump_format(input_ctx, 0, "input", 0);

    return 0;
}

int Transcoder::open_output(const std::string &filename) {

    output_ctx = avformat_alloc_context();
    output_ctx->oformat = av_guess_format(nullptr,".mp4",nullptr);
    if (avio_open2(&output_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr) < 0) {
        return EXIT_FAILURE;
    }

    return 0;
}

int Transcoder::init_encoder() {
    int ret = 0;

    AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264); // Why not h264 ?
    if (!codec) {
        return -1;
    }

    AVStream* stream = avformat_new_stream(output_ctx, codec);

    AVCodecParameters* codec_paramters = stream->codecpar;
    codec_paramters->codec_tag = 0;
    codec_paramters->codec_id = codec->id;
    codec_paramters->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_paramters->width = stream_ctx->dec_ctx->width;
    codec_paramters->height = stream_ctx->dec_ctx->height;
    codec_paramters->format = stream_ctx->dec_ctx->pix_fmt;

    stream_ctx->enc_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(stream_ctx->enc_ctx, codec_paramters);

    stream_ctx->enc_ctx->time_base = { 1, 90000 };

    ret = avcodec_open2(stream_ctx->enc_ctx, codec, nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s open output codec\n", av_err2str(ret));
        return ret;
    }

    ret = avformat_write_header(output_ctx, nullptr);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s write header\n", av_err2str(ret));
        return ret;
    }
    av_dump_format(output_ctx, -1, "out", 1);

    return 0;
}

int Transcoder::init_decoder() {
    int ret = 0;
    AVCodec *codec = avcodec_find_decoder(input_ctx->streams[video_index]->codecpar->codec_id);

    stream_ctx->dec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(stream_ctx->dec_ctx, input_ctx->streams[video_index]->codecpar);
    ret = avcodec_open2(stream_ctx->dec_ctx, codec, NULL);
    if (ret < 0) {
        std::cerr << "open codec error" << std::endl;
        return 1;
    }

    return 0;
}

int Transcoder::init_filtergraph(const std::string &filterstring) {
    char args[512];
    int ret = 0;
    const    AVFilter* buffersrc = avfilter_get_by_name("buffer");
    const   AVFilter* buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();

    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_NONE };

    filter_ctx->filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_ctx->filter_graph) {
        ret = AVERROR(ENOMEM);
        return ret;
    }
    AVRational time_base = input_ctx->streams[video_index]->time_base;

    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             stream_ctx->dec_ctx->width,
             stream_ctx->dec_ctx->height,
             stream_ctx->dec_ctx->pix_fmt,
             time_base.num,
             time_base.den,
             stream_ctx->dec_ctx->sample_aspect_ratio.num,
             stream_ctx->dec_ctx->sample_aspect_ratio.den);


    ret = avfilter_graph_create_filter(&filter_ctx->buffersrc_ctx, buffersrc, "in", args, nullptr, filter_ctx->filter_graph);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Cannot create buffer source\n");
        return ret;
    }

    ret = avfilter_graph_create_filter(&filter_ctx->buffersink_ctx, buffersink, "out", nullptr, nullptr, filter_ctx->filter_graph);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "Cannot create buffer sink\n");
        return ret;
    }

    av_opt_set_int_list(filter_ctx->buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "can not set output pixel format\n");
        return ret;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = filter_ctx->buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = filter_ctx->buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    if ((ret = avfilter_graph_parse_ptr(filter_ctx->filter_graph, filterstring.c_str(), &inputs, &outputs, nullptr)) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "parse filter graph error\n");
        return ret;
    }

    if ((ret = avfilter_graph_config(filter_ctx->filter_graph, nullptr)) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "config graph error\n");
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);

        return ret;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return 0;


}

void Transcoder::loop(int count) {
    int ret = 0;

    AVPacket* currPacket = av_packet_alloc();
    av_init_packet(currPacket);
    int cnt = 0;

    while (av_read_frame(input_ctx, currPacket) == 0 & cnt < count) {
        cnt++;
        std::cerr << "Frame :" << cnt << std::endl;

        //correct and rescale timestamps for mjpeg stream
        int64_t start_time = input_ctx->streams[video_index]->start_time;
        AVRational input_timebase = input_ctx->streams[video_index]->time_base;
        AVRational output_timebase = output_ctx->streams[video_index]->time_base;

        currPacket->pts = currPacket->pts - start_time;
        currPacket->dts = currPacket->dts - start_time;
        av_packet_rescale_ts(currPacket, input_timebase, output_timebase);


        ret = avcodec_send_packet(stream_ctx->dec_ctx, currPacket);

        while (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            ret = avcodec_receive_frame(stream_ctx->dec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }

            if (ret >= 0) {
                // processing one frame
                ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
                if (ret < 0) {
                    av_log(nullptr, AV_LOG_ERROR, "error add frame to buffer source \n");
                }
            }
            av_frame_free(&frame);
        }

    }

    // flush buffer src
    if ((ret = av_buffersrc_add_frame_flags(filter_ctx->buffersrc_ctx, nullptr, AV_BUFFERSRC_FLAG_KEEP_REF)) >= 0) {
        do {
            AVFrame* filter_frame = av_frame_alloc();
            ret = av_buffersink_get_frame(filter_ctx->buffersink_ctx, filter_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_frame_unref(filter_frame);
                break;
            }

            {
                int r = avcodec_send_frame(stream_ctx->enc_ctx, filter_frame);
                AVPacket *pkt = av_packet_alloc();
                av_init_packet(pkt);

                // write the filtered frame to output file
                while (r >= 0) {
                    r = avcodec_receive_packet(stream_ctx->enc_ctx, pkt);
                    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                        break;
                    }

                    av_interleaved_write_frame(output_ctx, pkt);
                }

                av_packet_unref(pkt);

            }

            av_frame_unref(filter_frame);
        } while (ret >= 0);
    } else {
        av_log(nullptr, AV_LOG_ERROR, "error add frame to buffer source\n");
    }

    av_packet_unref(currPacket);

    av_write_trailer(output_ctx);

}

int main(int argc, char **argv) {

    // Register everything
    av_register_all();
    avcodec_register_all();
    avformat_network_init();

    Transcoder transcoder;
    transcoder.open_stream("http://dmitry:r1ZtdJlI@97.68.27.42:1555/mjpeg/stream.cgi?chn=1");
    transcoder.init_decoder();
    transcoder.open_output("dmitry.mp4");
    transcoder.init_encoder();
    transcoder.init_filtergraph("drawtext=text='Dmitry': fontcolor=white: fontsize=24: x=10: y=(h-text_h)-10");

    //read only 100 frames, because I have really bad connection.
    transcoder.loop(100);

    avformat_network_deinit();

    return (EXIT_SUCCESS);
}
