#include "FfmpegProcessor.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace RoadTripMovie::FfmpegProcessor
{
    namespace
    {
        using Path = std::filesystem::path;

        struct InputFormatDeleter
        {
            void operator()(AVFormatContext* value) const noexcept
            {
                if (value)
                    avformat_close_input(&value);
            }
        };

        struct OutputFormatDeleter
        {
            void operator()(AVFormatContext* value) const noexcept
            {
                if (value)
                {
                    if (value->pb)
                        avio_closep(&value->pb);
                    avformat_free_context(value);
                }
            }
        };

        struct CodecDeleter
        {
            void operator()(AVCodecContext* value) const noexcept
            {
                avcodec_free_context(&value);
            }
        };

        struct PacketDeleter
        {
            void operator()(AVPacket* value) const noexcept
            {
                av_packet_free(&value);
            }
        };

        struct FrameDeleter
        {
            void operator()(AVFrame* value) const noexcept
            {
                av_frame_free(&value);
            }
        };

        struct FilterGraphDeleter
        {
            void operator()(AVFilterGraph* value) const noexcept
            {
                avfilter_graph_free(&value);
            }
        };

        using InputFormat = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
        using OutputFormat = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
        using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;
        using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
        using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
        using FilterGraph = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
        [[noreturn]] void ThrowAvError(int code, std::string const& action)
        {
            char text[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(code, text, sizeof(text));
            throw std::runtime_error(action + ": " + text);
        }

        void Check(int code, std::string const& action)
        {
            if (code < 0)
                ThrowAvError(code, action);
        }

        std::string Utf8(Path const& path)
        {
            auto value = path.u8string();
            return { reinterpret_cast<char const*>(value.data()), value.size() };
        }

        InputFormat OpenInput(Path const& path)
        {
            AVFormatContext* format{};
            auto file = Utf8(path);
            Check(avformat_open_input(&format, file.c_str(), nullptr, nullptr),
                "Could not open " + file);
            InputFormat result(format);
            Check(avformat_find_stream_info(format, nullptr),
                "Could not inspect " + file);
            return result;
        }

        OutputFormat CreateOutput(Path const& path)
        {
            AVFormatContext* format{};
            auto file = Utf8(path);
            Check(avformat_alloc_output_context2(&format, nullptr, "mp4", file.c_str()),
                "Could not create MP4 output");
            if (!format)
                throw std::runtime_error("FFmpeg did not create an MP4 output context.");
            return OutputFormat(format);
        }

        AVStream* AddStream(AVFormatContext* output, AVCodecContext* codec)
        {
            auto stream = avformat_new_stream(output, nullptr);
            if (!stream)
                throw std::bad_alloc();
            stream->time_base = codec->time_base;
            Check(avcodec_parameters_from_context(stream->codecpar, codec),
                "Could not configure an output stream");
            return stream;
        }

        void OpenOutputFile(AVFormatContext* output, Path const& path)
        {
            if ((output->oformat->flags & AVFMT_NOFILE) == 0)
                Check(avio_open(&output->pb, Utf8(path).c_str(), AVIO_FLAG_WRITE),
                    "Could not open the output file");
            Check(avformat_write_header(output, nullptr),
                "Could not write the MP4 header");
        }

        void CloseOutputFile(AVFormatContext* output)
        {
            Check(av_write_trailer(output), "Could not finish the MP4 file");
            if ((output->oformat->flags & AVFMT_NOFILE) == 0 && output->pb)
                Check(avio_closep(&output->pb), "Could not close the MP4 file");
        }

        AVCodecContext* CreateEncoder(
            AVCodecID id,
            AVFormatContext* output,
            int width,
            int height,
            int fps)
        {
            auto encoder = avcodec_find_encoder(id);
            if (!encoder)
                throw std::runtime_error("The FFmpeg build does not provide a required encoder.");
            Codec context(avcodec_alloc_context3(encoder));
            if (!context)
                throw std::bad_alloc();

            if (id == AV_CODEC_ID_MPEG4)
            {
                context->width = width;
                context->height = height;
                context->time_base = AVRational{ 1, fps };
                context->framerate = AVRational{ fps, 1 };
                context->pix_fmt = AV_PIX_FMT_YUV420P;
                context->bit_rate = 5'000'000;
                context->gop_size = fps * 2;
                context->max_b_frames = 0;
                context->thread_count = 2;
            }
            else
            {
                context->sample_rate = 44100;
                context->sample_fmt = AV_SAMPLE_FMT_FLTP;
                context->time_base = AVRational{ 1, context->sample_rate };
                context->bit_rate = 192'000;
                av_channel_layout_default(&context->ch_layout, 2);
            }

            if (output->oformat->flags & AVFMT_GLOBALHEADER)
                context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            Check(avcodec_open2(context.get(), encoder, nullptr),
                "Could not open an FFmpeg encoder");
            return context.release();
        }

        AVFilterContext* CreateSource(
            AVFilterGraph* graph,
            char const* filterName,
            char const* instanceName,
            std::string const& arguments)
        {
            auto filter = avfilter_get_by_name(filterName);
            if (!filter)
                throw std::runtime_error(std::string("FFmpeg filter not found: ") + filterName);
            AVFilterContext* source{};
            Check(avfilter_graph_create_filter(
                    &source, filter, instanceName, arguments.c_str(), nullptr, graph),
                std::string("Could not create FFmpeg source ") + filterName);
            return source;
        }

        AVFilterContext* CreateSink(
            AVFilterGraph* graph,
            char const* filterName,
            char const* instanceName)
        {
            auto filter = avfilter_get_by_name(filterName);
            if (!filter)
                throw std::runtime_error(std::string("FFmpeg filter not found: ") + filterName);
            AVFilterContext* sink{};
            Check(avfilter_graph_create_filter(&sink, filter, instanceName, nullptr, nullptr, graph),
                std::string("Could not create FFmpeg sink ") + filterName);
            return sink;
        }

        AVFilterContext* CreateMediaSource(
            AVFilterGraph* graph,
            char const* filterName,
            char const* instanceName,
            Path const& path,
            int loop = 0)
        {
            auto filter = avfilter_get_by_name(filterName);
            if (!filter)
                throw std::runtime_error(std::string("FFmpeg filter not found: ") + filterName);
            auto source = avfilter_graph_alloc_filter(graph, filter, instanceName);
            if (!source)
                throw std::bad_alloc();
            auto filename = Utf8(path);
            Check(av_opt_set(source, "filename", filename.c_str(), AV_OPT_SEARCH_CHILDREN),
                std::string("Could not set the FFmpeg source path for ") + filterName);
            if (loop != 0)
                Check(av_opt_set_int(source, "loop", loop, AV_OPT_SEARCH_CHILDREN),
                    std::string("Could not set the FFmpeg source loop for ") + filterName);
            Check(avfilter_init_str(source, nullptr),
                std::string("Could not initialize FFmpeg source ") + filterName);
            return source;
        }

        AVFilterInOut* NewEndpoint(
            char const* name,
            AVFilterContext* context,
            AVFilterInOut* next)
        {
            auto endpoint = avfilter_inout_alloc();
            if (!endpoint)
                throw std::bad_alloc();
            endpoint->name = av_strdup(name);
            if (!endpoint->name)
            {
                avfilter_inout_free(&endpoint);
                throw std::bad_alloc();
            }
            endpoint->filter_ctx = context;
            endpoint->pad_idx = 0;
            endpoint->next = next;
            return endpoint;
        }

        void ParseFilterGraph(
            AVFilterGraph* graph,
            std::string const& description,
            std::vector<std::pair<std::string, AVFilterContext*>> const& sources,
            std::vector<std::pair<std::string, AVFilterContext*>> const& sinks)
        {
            AVFilterInOut* sourceEndpoints{};
            AVFilterInOut* sinkEndpoints{};
            for (auto const& [label, context] : sources)
                sourceEndpoints = NewEndpoint(label.c_str(), context, sourceEndpoints);
            for (auto const& [label, context] : sinks)
                sinkEndpoints = NewEndpoint(label.c_str(), context, sinkEndpoints);
            auto result = avfilter_graph_parse_ptr(
                graph, description.c_str(), &sinkEndpoints, &sourceEndpoints, nullptr);
            avfilter_inout_free(&sourceEndpoints);
            avfilter_inout_free(&sinkEndpoints);
            Check(result, "Could not connect FFmpeg filters");
            Check(avfilter_graph_config(graph, nullptr),
                "Could not configure FFmpeg filters");
        }

        void WriteEncoded(
            AVCodecContext* encoder,
            AVStream* stream,
            AVFormatContext* output,
            AVFrame* frame,
            std::stop_token cancellation)
        {
            if (cancellation.stop_requested())
                throw std::runtime_error("Movie creation was cancelled.");
            Check(avcodec_send_frame(encoder, frame), "Could not submit an encoded frame");
            Packet packet(av_packet_alloc());
            if (!packet)
                throw std::bad_alloc();
            for (;;)
            {
                auto result = avcodec_receive_packet(encoder, packet.get());
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    break;
                Check(result, "Could not receive an encoded packet");
                av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
                packet->stream_index = stream->index;
                Check(av_write_frame(output, packet.get()), "Could not write an encoded packet");
                av_packet_unref(packet.get());
            }
        }

        void FlushEncoder(
            AVCodecContext* encoder,
            AVStream* stream,
            AVFormatContext* output)
        {
            Check(avcodec_send_frame(encoder, nullptr), "Could not flush an encoder");
            Packet packet(av_packet_alloc());
            if (!packet)
                throw std::bad_alloc();
            for (;;)
            {
                auto result = avcodec_receive_packet(encoder, packet.get());
                if (result == AVERROR_EOF || result == AVERROR(EAGAIN))
                    break;
                Check(result, "Could not receive a flushed packet");
                av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
                packet->stream_index = stream->index;
                Check(av_write_frame(output, packet.get()), "Could not write a flushed packet");
                av_packet_unref(packet.get());
            }
        }

        void EncodeSink(
            AVFilterContext* sink,
            AVCodecContext* encoder,
            AVStream* stream,
            AVFormatContext* output,
            std::stop_token cancellation)
        {
            Frame frame(av_frame_alloc());
            if (!frame)
                throw std::bad_alloc();
            for (;;)
            {
                auto result = av_buffersink_get_frame(sink, frame.get());
                if (result == AVERROR_EOF)
                    break;
                Check(result, "Could not read a filtered media frame");
                frame->pts = av_rescale_q(
                    frame->pts, av_buffersink_get_time_base(sink), encoder->time_base);
                WriteEncoded(encoder, stream, output, frame.get(), cancellation);
                av_frame_unref(frame.get());
            }
            FlushEncoder(encoder, stream, output);
        }

        std::string VideoFilterDescription(
            PipelineSettings const& settings,
            double duration,
            bool photo)
        {
            return std::string("[video]") +
                (photo ? "loop=loop=-1:size=1:start=0," : "") +
                "fps=fps=" + std::to_string(settings.fps) +
                ",scale=" + std::to_string(settings.width) + ":" +
                std::to_string(settings.height) + ":force_original_aspect_ratio=decrease" +
                ",pad=" + std::to_string(settings.width) + ":" +
                std::to_string(settings.height) + ":(ow-iw)/2:(oh-ih)/2" +
                ",setsar=1,format=yuv420p,trim=duration=" + std::to_string(duration) +
                ",setpts=PTS-STARTPTS[video_out]";
        }

        std::string SegmentAudioFilterDescription(
            bool hasAudio,
            PipelineSettings const& settings,
            double duration)
        {
            auto silence = "[silence]atrim=duration=" + std::to_string(duration) +
                ",asetpts=PTS-STARTPTS[silent_audio];";
            if (!hasAudio)
                return silence + "[silent_audio]aformat=sample_fmts=fltp:sample_rates=44100:"
                    "channel_layouts=stereo,asetpts=PTS-STARTPTS[audio_out]";
            return "[source]atrim=duration=" + std::to_string(duration) +
                ",asetpts=PTS-STARTPTS,volume=" +
                std::to_string(settings.videoAudioVolume) + "[source_audio];" +
                silence +
                "[source_audio][silent_audio]amix=inputs=2:duration=longest:"
                "dropout_transition=0:normalize=0,atrim=duration=" +
                std::to_string(duration) +
                ",aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,"
                "asetpts=PTS-STARTPTS[audio_out]";
        }

        FilterGraph CreateGraph()
        {
            FilterGraph graph(avfilter_graph_alloc());
            if (!graph)
                throw std::bad_alloc();
            graph->nb_threads = 1;
            return graph;
        }

        void CreateAudioOutput(
            OutputFormat const& output,
            Codec& encoder,
            AVStream*& stream)
        {
            encoder.reset(CreateEncoder(AV_CODEC_ID_AAC, output.get(), 0, 0, 0));
            stream = AddStream(output.get(), encoder.get());
        }

        int FindStream(AVFormatContext* input, AVMediaType type)
        {
            auto index = av_find_best_stream(input, type, -1, -1, nullptr, 0);
            if (index == AVERROR_STREAM_NOT_FOUND)
                return -1;
            Check(index, "Could not find a media stream");
            return index;
        }

        void CheckCancelled(std::stop_token cancellation)
        {
            if (cancellation.stop_requested())
                throw std::runtime_error("Movie creation was cancelled.");
        }
    }

    void RenderSegment(
        MediaItem const& item,
        PipelineSettings const& settings,
        Path const& target,
        std::stop_token cancellation)
    {
        CheckCancelled(cancellation);
        auto graph = CreateGraph();
        auto videoSource = CreateMediaSource(
            graph.get(), "movie", "video_source", item.file);
        auto silenceSource = CreateSource(
            graph.get(), "anullsrc", "silence", "r=44100:cl=stereo");
        AVFilterContext* audioSource{};
        if (item.hasAudio)
            audioSource = CreateMediaSource(
                graph.get(), "amovie", "audio_source", item.file);

        auto videoSink = CreateSink(graph.get(), "buffersink", "video_sink");
        auto audioSink = CreateSink(graph.get(), "abuffersink", "audio_sink");
        std::vector<std::pair<std::string, AVFilterContext*>> sources{
            { "video", videoSource }, { "silence", silenceSource }
        };
        if (audioSource)
            sources.emplace_back("source", audioSource);
        ParseFilterGraph(
            graph.get(),
            VideoFilterDescription(settings, item.duration, item.photo) + ";" +
                SegmentAudioFilterDescription(item.hasAudio, settings, item.duration),
            sources,
            { { "video_out", videoSink }, { "audio_out", audioSink } });

        auto output = CreateOutput(target);
        Codec video(CreateEncoder(
            AV_CODEC_ID_MPEG4, output.get(), settings.width, settings.height, settings.fps));
        auto videoStream = AddStream(output.get(), video.get());
        Codec audio;
        AVStream* audioStream{};
        CreateAudioOutput(output, audio, audioStream);
        OpenOutputFile(output.get(), target);

        EncodeSink(videoSink, video.get(), videoStream, output.get(), cancellation);
        EncodeSink(audioSink, audio.get(), audioStream, output.get(), cancellation);
        CloseOutputFile(output.get());
    }

    void Concatenate(
        std::vector<Path> const& segments,
        Path const& target,
        std::stop_token cancellation)
    {
        if (segments.empty())
            throw std::invalid_argument("There are no rendered segments to concatenate.");
        auto firstInput = OpenInput(segments.front());
        auto output = CreateOutput(target);
        std::vector<int> streamMap(firstInput->nb_streams, -1);
        std::vector<int64_t> streamEnds(firstInput->nb_streams, 0);
        for (unsigned int index = 0; index < firstInput->nb_streams; ++index)
        {
            auto inputStream = firstInput->streams[index];
            auto outputStream = avformat_new_stream(output.get(), nullptr);
            if (!outputStream)
                throw std::bad_alloc();
            Check(avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar),
                "Could not copy a segment stream");
            outputStream->time_base = inputStream->time_base;
            streamMap[index] = static_cast<int>(outputStream->index);
        }
        OpenOutputFile(output.get(), target);
        for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
        {
            CheckCancelled(cancellation);
            auto input = segmentIndex == 0 ? std::move(firstInput) : OpenInput(segments[segmentIndex]);
            if (input->nb_streams != streamMap.size())
                throw std::runtime_error("Rendered segment stream layouts do not match.");
            auto segmentOffsets = streamEnds;
            std::vector<bool> hasFirstTimestamp(streamEnds.size(), false);
            Packet packet(av_packet_alloc());
            if (!packet)
                throw std::bad_alloc();
            for (;;)
            {
                auto readResult = av_read_frame(input.get(), packet.get());
                if (readResult == AVERROR_EOF)
                    break;
                Check(readResult, "Could not read a rendered segment");
                CheckCancelled(cancellation);
                auto inputIndex = packet->stream_index;
                if (inputIndex < 0 || static_cast<std::size_t>(inputIndex) >= streamMap.size())
                {
                    av_packet_unref(packet.get());
                    continue;
                }
                auto outputIndex = streamMap[static_cast<std::size_t>(inputIndex)];
                auto outputStream = output->streams[outputIndex];
                av_packet_rescale_ts(
                    packet.get(), input->streams[inputIndex]->time_base, outputStream->time_base);
                auto streamIndex = static_cast<std::size_t>(inputIndex);
                auto offset = segmentOffsets[streamIndex];
                auto timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                if (!hasFirstTimestamp[streamIndex] && timestamp != AV_NOPTS_VALUE)
                {
                    if (segmentIndex != 0)
                        offset -= timestamp;
                    segmentOffsets[streamIndex] = offset;
                    hasFirstTimestamp[streamIndex] = true;
                }
                if (packet->pts != AV_NOPTS_VALUE)
                    packet->pts += offset;
                if (packet->dts != AV_NOPTS_VALUE)
                    packet->dts += offset;
                packet->stream_index = outputIndex;
                auto end = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                if (end != AV_NOPTS_VALUE)
                    streamEnds[streamIndex] =
                        std::max(streamEnds[streamIndex], end + packet->duration);
                Check(av_write_frame(output.get(), packet.get()),
                    "Could not concatenate a media packet");
                av_packet_unref(packet.get());
            }
        }
        CloseOutputFile(output.get());
    }

    void MixMusic(
        Path const& joinedVideo,
        Path const& music,
        Path const& target,
        double musicVolume,
        double duration,
        std::stop_token cancellation)
    {
        CheckCancelled(cancellation);
        auto input = OpenInput(joinedVideo);
        auto videoIndex = FindStream(input.get(), AVMEDIA_TYPE_VIDEO);
        auto audioIndex = FindStream(input.get(), AVMEDIA_TYPE_AUDIO);
        if (videoIndex < 0 || audioIndex < 0)
            throw std::runtime_error("The concatenated movie is missing its video or audio stream.");

        auto output = CreateOutput(target);
        auto videoStream = avformat_new_stream(output.get(), nullptr);
        if (!videoStream)
            throw std::bad_alloc();
        Check(avcodec_parameters_copy(
                videoStream->codecpar, input->streams[videoIndex]->codecpar),
            "Could not copy the movie video stream");
        videoStream->time_base = input->streams[videoIndex]->time_base;

        Codec audioEncoder;
        AVStream* audioStream{};
        CreateAudioOutput(output, audioEncoder, audioStream);
        OpenOutputFile(output.get(), target);

        auto graph = CreateGraph();
        auto sourceAudio = CreateMediaSource(
            graph.get(), "amovie", "movie_audio", joinedVideo);
        auto musicAudio = CreateMediaSource(
            graph.get(), "amovie", "music_audio", music, std::numeric_limits<int>::max());
        auto sink = CreateSink(graph.get(), "abuffersink", "mixed_audio_sink");
        auto description = "[movie_audio]asetpts=PTS-STARTPTS[movie];" +
            std::string("[music_audio]volume=") + std::to_string(musicVolume) +
            "[music];[movie][music]amix=inputs=2:duration=first:dropout_transition=0:"
            "normalize=0,atrim=duration=" + std::to_string(duration) +
            ",aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,"
            "asetpts=PTS-STARTPTS[audio_out]";
        ParseFilterGraph(
            graph.get(),
            description,
            { { "movie_audio", sourceAudio }, { "music_audio", musicAudio } },
            { { "audio_out", sink } });

        Packet packet(av_packet_alloc());
        if (!packet)
            throw std::bad_alloc();
        for (;;)
        {
            auto readResult = av_read_frame(input.get(), packet.get());
            if (readResult == AVERROR_EOF)
                break;
            Check(readResult, "Could not read the concatenated movie");
            CheckCancelled(cancellation);
            if (packet->stream_index == videoIndex)
            {
                av_packet_rescale_ts(
                    packet.get(), input->streams[videoIndex]->time_base, videoStream->time_base);
                packet->stream_index = videoStream->index;
                Check(av_write_frame(output.get(), packet.get()),
                    "Could not copy a movie video packet");
            }
            av_packet_unref(packet.get());
        }
        EncodeSink(sink, audioEncoder.get(), audioStream, output.get(), cancellation);
        CloseOutputFile(output.get());
    }
}
