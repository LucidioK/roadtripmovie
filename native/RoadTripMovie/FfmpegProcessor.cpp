#include "FfmpegProcessor.h"
#include "CityLookup.h"
#include "MapThumbnail.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
}

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace RoadTripMovie::FfmpegProcessor
{
    namespace
    {
        using Path = std::filesystem::path;

        class TemporaryDirectoryCleanup
        {
        public:
            explicit TemporaryDirectoryCleanup(Path path) : path_(std::move(path)) {}

            ~TemporaryDirectoryCleanup()
            {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
            }

            TemporaryDirectoryCleanup(TemporaryDirectoryCleanup const&) = delete;
            TemporaryDirectoryCleanup& operator=(TemporaryDirectoryCleanup const&) = delete;

        private:
            Path path_;
        };

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

        std::mutex audioValidationMutex;
        std::mutex audioLogMutex;
        std::string* capturedAudioErrors{};

        void CaptureAudioLog(void* context, int level, char const* format, va_list arguments)
        {
            if (level <= AV_LOG_ERROR)
            {
                char message[2048]{};
                va_list copy;
                va_copy(copy, arguments);
                auto length = std::vsnprintf(message, sizeof(message), format, copy);
                va_end(copy);
                if (length > 0)
                {
                    std::scoped_lock lock(audioLogMutex);
                    if (capturedAudioErrors)
                    {
                        constexpr std::size_t maxCapturedAudioErrors = 4096;
                        auto remaining = maxCapturedAudioErrors - capturedAudioErrors->size();
                        capturedAudioErrors->append(
                            message,
                            std::min({ static_cast<std::size_t>(length),
                                sizeof(message) - 1, remaining }));
                    }
                }
            }
            av_log_default_callback(context, level, format, arguments);
        }

        class AudioLogCapture
        {
        public:
            AudioLogCapture() : validationLock_(audioValidationMutex)
            {
                std::scoped_lock lock(audioLogMutex);
                capturedAudioErrors = &errors_;
                av_log_set_callback(CaptureAudioLog);
            }

            ~AudioLogCapture()
            {
                av_log_set_callback(av_log_default_callback);
                std::scoped_lock lock(audioLogMutex);
                capturedAudioErrors = nullptr;
            }

            AudioLogCapture(AudioLogCapture const&) = delete;
            AudioLogCapture& operator=(AudioLogCapture const&) = delete;

            std::string Errors()
            {
                std::scoped_lock lock(audioLogMutex);
                return errors_;
            }

        private:
            std::unique_lock<std::mutex> validationLock_;
            std::string errors_;
        };

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

        struct ExifMetadata
        {
            std::optional<int> orientation;
            std::optional<GeoLocation> location;
        };

        std::optional<ExifMetadata> ReadJpegExif(Path const& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return std::nullopt;

            auto readByte = [&input]() -> std::optional<uint8_t>
            {
                char value{};
                if (!input.get(value))
                    return std::nullopt;
                return static_cast<uint8_t>(static_cast<unsigned char>(value));
            };
            if (readByte() != 0xff || readByte() != 0xd8)
                return std::nullopt;

            while (input)
            {
                auto prefix = readByte();
                if (!prefix)
                    break;
                if (*prefix != 0xff)
                    continue;

                std::optional<uint8_t> marker;
                do
                {
                    marker = readByte();
                } while (marker && *marker == 0xff);
                if (!marker || *marker == 0xda || *marker == 0xd9)
                    break;
                if (*marker == 0x01 || (*marker >= 0xd0 && *marker <= 0xd7))
                    continue;

                auto high = readByte();
                auto low = readByte();
                if (!high || !low)
                    break;
                auto length = (static_cast<unsigned int>(*high) << 8) | *low;
                if (length < 2)
                    break;
                auto payloadLength = length - 2;
                if (*marker != 0xe1 || payloadLength < 14)
                {
                    input.seekg(payloadLength, std::ios::cur);
                    continue;
                }

                std::vector<uint8_t> payload(payloadLength);
                if (!input.read(
                        reinterpret_cast<char*>(payload.data()),
                        static_cast<std::streamsize>(payload.size())))
                    break;
                constexpr uint8_t exifSignature[]{ 'E', 'x', 'i', 'f', 0, 0 };
                if (!std::equal(std::begin(exifSignature), std::end(exifSignature), payload.begin()))
                    continue;

                auto tiff = std::span<uint8_t const>(payload).subspan(6);
                if (tiff.size() < 8)
                    continue;
                bool littleEndian;
                if (tiff[0] == 'I' && tiff[1] == 'I')
                    littleEndian = true;
                else if (tiff[0] == 'M' && tiff[1] == 'M')
                    littleEndian = false;
                else
                    continue;

                auto read16 = [littleEndian, tiff](std::size_t offset) -> std::optional<uint16_t>
                {
                    if (offset > tiff.size() || tiff.size() - offset < 2)
                        return std::nullopt;
                    auto first = tiff[offset];
                    auto second = tiff[offset + 1];
                    return littleEndian
                        ? static_cast<uint16_t>(first | (static_cast<uint16_t>(second) << 8))
                        : static_cast<uint16_t>((static_cast<uint16_t>(first) << 8) | second);
                };
                auto read32 = [littleEndian, tiff](std::size_t offset) -> std::optional<uint32_t>
                {
                    if (offset > tiff.size() || tiff.size() - offset < 4)
                        return std::nullopt;
                    uint32_t value{};
                    for (std::size_t index = 0; index < 4; ++index)
                    {
                        auto shift = littleEndian ? index * 8 : (3 - index) * 8;
                        value |= static_cast<uint32_t>(tiff[offset + index]) << shift;
                    }
                    return value;
                };

                if (read16(2) != 42)
                    continue;
                auto directoryOffset = read32(4);
                if (!directoryOffset || *directoryOffset > tiff.size())
                    continue;
                auto entryCount = read16(*directoryOffset);
                if (!entryCount)
                    continue;
                auto entriesOffset = static_cast<std::size_t>(*directoryOffset) + 2;
                ExifMetadata metadata;
                std::optional<uint32_t> gpsDirectoryOffset;
                for (uint16_t index = 0; index < *entryCount; ++index)
                {
                    auto entryOffset = entriesOffset + static_cast<std::size_t>(index) * 12;
                    auto tag = read16(entryOffset);
                    if (!tag)
                        break;
                    if (*tag == 0x0112)
                    {
                        auto type = read16(entryOffset + 2);
                        auto count = read32(entryOffset + 4);
                        if (type && *type == 3 && count && *count == 1)
                        {
                            auto orientation = read16(entryOffset + 8);
                            if (orientation && *orientation >= 1 && *orientation <= 8)
                                metadata.orientation = *orientation;
                        }
                    }
                    else if (*tag == 0x8825)
                    {
                        auto offset = read32(entryOffset + 8);
                        if (offset)
                            gpsDirectoryOffset = *offset;
                    }
                }
                if (!gpsDirectoryOffset)
                    return metadata;

                auto gpsEntryCount = read16(*gpsDirectoryOffset);
                if (!gpsEntryCount)
                    return metadata;
                std::optional<char> latitudeReference;
                std::optional<char> longitudeReference;
                std::optional<uint32_t> latitudeOffset;
                std::optional<uint32_t> longitudeOffset;
                std::optional<uint32_t> latitudeCount;
                std::optional<uint32_t> longitudeCount;
                auto gpsEntriesOffset = static_cast<std::size_t>(*gpsDirectoryOffset) + 2;
                for (uint16_t index = 0; index < *gpsEntryCount; ++index)
                {
                    auto entryOffset = gpsEntriesOffset + static_cast<std::size_t>(index) * 12;
                    auto tag = read16(entryOffset);
                    auto type = read16(entryOffset + 2);
                    auto count = read32(entryOffset + 4);
                    if (!tag || !type || !count)
                        break;

                    if ((*tag == 1 || *tag == 3) && *type == 2 && *count >= 1)
                    {
                        auto valueOffset = entryOffset + 8;
                        if (valueOffset < tiff.size())
                        {
                            auto reference = static_cast<char>(tiff[valueOffset]);
                            if (*tag == 1)
                                latitudeReference = reference;
                            else
                                longitudeReference = reference;
                        }
                    }
                    else if ((*tag == 2 || *tag == 4) && *type == 5 && *count >= 3)
                    {
                        auto offset = read32(entryOffset + 8);
                        if (*tag == 2)
                        {
                            latitudeOffset = offset;
                            latitudeCount = *count;
                        }
                        else
                        {
                            longitudeOffset = offset;
                            longitudeCount = *count;
                        }
                    }
                }

                auto readCoordinate = [&](uint32_t offset, uint32_t count)
                    -> std::optional<double>
                {
                    if (count < 3)
                        return std::nullopt;
                    double components[3]{};
                    for (std::size_t component = 0; component < 3; ++component)
                    {
                        auto numerator = read32(static_cast<std::size_t>(offset) + component * 8);
                        auto denominator = read32(static_cast<std::size_t>(offset) + component * 8 + 4);
                        if (!numerator || !denominator || *denominator == 0)
                            return std::nullopt;
                        components[component] =
                            static_cast<double>(*numerator) / *denominator;
                    }
                    return components[0] + components[1] / 60.0 + components[2] / 3600.0;
                };
                if (latitudeReference && longitudeReference &&
                    latitudeOffset && longitudeOffset &&
                    latitudeCount && longitudeCount)
                {
                    auto latitude = readCoordinate(*latitudeOffset, *latitudeCount);
                    auto longitude = readCoordinate(*longitudeOffset, *longitudeCount);
                    if (latitude && longitude)
                    {
                        if (*latitudeReference == 'S')
                            *latitude = -*latitude;
                        if (*longitudeReference == 'W')
                            *longitude = -*longitude;
                        if (std::abs(*latitude) <= 90.0 && std::abs(*longitude) <= 180.0)
                            metadata.location = GeoLocation{ *latitude, *longitude };
                    }
                }
                return metadata;
            }
            return std::nullopt;
        }

        std::string OrientationFilter(Path const& path, bool photo)
        {
            std::optional<int> exifOrientation;
            auto extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            if (photo && (extension == L".jpg" || extension == L".jpeg"))
            {
                if (auto exif = ReadJpegExif(path))
                    exifOrientation = exif->orientation;
            }

            auto input = OpenInput(path);
            for (unsigned int index = 0; index < input->nb_streams; ++index)
            {
                auto stream = input->streams[index];
                if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                    continue;

                auto matrix = av_packet_side_data_get(
                    stream->codecpar->coded_side_data,
                    stream->codecpar->nb_coded_side_data,
                    AV_PKT_DATA_DISPLAYMATRIX);
                if (matrix && matrix->size >= 9 * sizeof(int32_t))
                {
                    auto angle = av_display_rotation_get(
                        reinterpret_cast<int32_t const*>(matrix->data));
                    if (std::isfinite(angle))
                    {
                        auto quarterTurns = static_cast<int>(
                            std::lround(angle / 90.0)) % 4;
                        if (quarterTurns < 0)
                            quarterTurns += 4;
                        if (quarterTurns == 1)
                            return "transpose=cclock,";
                        if (quarterTurns == 2)
                            return "hflip,vflip,";
                        if (quarterTurns == 3)
                            return "transpose=clock,";
                    }
                }

                auto rotation = av_dict_get(stream->metadata, "rotate", nullptr, 0);
                if (rotation)
                {
                    char* end{};
                    auto angle = std::strtod(rotation->value, &end);
                    if (end != rotation->value && *end == '\0' && std::isfinite(angle))
                    {
                        auto quarterTurns = static_cast<int>(
                            std::lround(angle / 90.0)) % 4;
                        if (quarterTurns < 0)
                            quarterTurns += 4;
                        if (quarterTurns == 1)
                            return "transpose=cclock,";
                        if (quarterTurns == 2)
                            return "hflip,vflip,";
                        if (quarterTurns == 3)
                            return "transpose=clock,";
                    }
                }
                break;
            }

            if (!exifOrientation || *exifOrientation == 1)
                return {};
            switch (*exifOrientation)
            {
            case 2: return "hflip,";
            case 3: return "hflip,vflip,";
            case 4: return "vflip,";
            case 5: return "transpose=cclock_flip,";
            case 6: return "transpose=clock,";
            case 7: return "transpose=clock_flip,";
            case 8: return "transpose=cclock,";
            default: return {};
            }
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

        void RequireFilter(char const* name)
        {
            if (!avfilter_get_by_name(name))
                throw std::runtime_error(
                    std::string("The loaded FFmpeg build does not provide the required '") +
                    name + "' filter.");
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
            if (result == AVERROR_FILTER_NOT_FOUND)
                throw std::runtime_error(
                    "Could not connect FFmpeg filters: a filter is not registered. "
                    "Graph: " + description);
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
            auto result = avcodec_send_frame(encoder, frame);
            if (result < 0)
            {
                auto mediaType = encoder->codec_type == AVMEDIA_TYPE_AUDIO ? "audio" : "video";
                auto action = std::string("Could not submit an encoded ") + mediaType +
                    " frame to " + avcodec_get_name(encoder->codec_id);
                if (encoder->codec_type == AVMEDIA_TYPE_AUDIO)
                    action += " (samples=" + std::to_string(frame->nb_samples) +
                        ", required=" + std::to_string(encoder->frame_size) +
                        ", format=" + std::to_string(frame->format) + ")";
                else
                    action += " (size=" + std::to_string(frame->width) + "x" +
                        std::to_string(frame->height) + ", format=" +
                        std::to_string(frame->format) + ")";
                ThrowAvError(result, action);
            }
            Packet packet(av_packet_alloc());
            if (!packet)
                throw std::bad_alloc();
            for (;;)
            {
                result = avcodec_receive_packet(encoder, packet.get());
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
            bool photo,
            std::string const& orientation,
            bool hasMap,
            bool hasCityLabel)
        {
            auto description = std::string("[video]") +
                (photo ? "loop=loop=-1:size=1:start=0," : "") +
                orientation +
                "fps=fps=" + std::to_string(settings.fps) +
                ",scale=" + std::to_string(settings.width) + ":" +
                std::to_string(settings.height) + ":force_original_aspect_ratio=decrease" +
                ",pad=" + std::to_string(settings.width) + ":" +
                std::to_string(settings.height) + ":(ow-iw)/2:(oh-ih)/2" +
                ",setsar=1,format=yuv420p[base_video]";
            auto currentVideo = std::string("base_video");
            if (hasMap)
            {
                description += ";[map]loop=loop=-1:size=1:start=0,scale=" +
                    std::to_string(settings.mapWidth) + ":" +
                    std::to_string(settings.mapHeight) +
                    "[map_image];[base_video][map_image]overlay=x=W-w-12:y=12:"
                    "shortest=1[video_with_map]";
                currentVideo = "video_with_map";
            }
            if (hasCityLabel)
            {
                description += ";[city]loop=loop=-1:size=1:start=0,format=rgba[city_image];[" +
                    currentVideo + "][city_image]overlay=x=(W-w)/2:y=H-h:shortest=1"
                    "[video_with_city]";
                currentVideo = "video_with_city";
            }
            description += ";[" + currentVideo + "]trim=duration=";
            description += std::to_string(duration) +
                ",setpts=PTS-STARTPTS[video_out]";
            return description;
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
                    "channel_layouts=stereo,asetpts=PTS-STARTPTS,"
                    "asetnsamples=n=1024:p=1[audio_out]";
            return "[source]atrim=duration=" + std::to_string(duration) +
                ",asetpts=PTS-STARTPTS,volume=" +
                std::to_string(settings.videoAudioVolume) + "[source_audio];" +
                silence +
                "[source_audio][silent_audio]amix=inputs=2:duration=longest:"
                "dropout_transition=0:normalize=0,atrim=duration=" +
                std::to_string(duration) +
                ",aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo,"
                "asetpts=PTS-STARTPTS,asetnsamples=n=1024:p=1[audio_out]";
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

        std::optional<GeoLocation> ParseIso6709(std::string const& raw)
        {
            auto first = raw.find_first_of("+-");
            if (first == std::string::npos)
                return std::nullopt;
            char* latitudeEnd{};
            auto latitude = std::strtod(raw.c_str() + first, &latitudeEnd);
            if (latitudeEnd == raw.c_str() + first ||
                (*latitudeEnd != '+' && *latitudeEnd != '-'))
                return std::nullopt;
            char* longitudeEnd{};
            auto longitude = std::strtod(latitudeEnd, &longitudeEnd);
            if (longitudeEnd == latitudeEnd || !std::isfinite(latitude) ||
                !std::isfinite(longitude) || std::abs(latitude) > 90.0 ||
                std::abs(longitude) > 180.0)
                return std::nullopt;
            return GeoLocation{ latitude, longitude };
        }

        std::optional<GeoLocation> LocationFromDictionary(AVDictionary* metadata)
        {
            for (auto key : {
                     "com.apple.quicktime.location.ISO6709",
                     "location",
                     "location-eng" })
            {
                auto value = av_dict_get(metadata, key, nullptr, 0);
                if (!value || !value->value)
                    continue;
                if (auto location = ParseIso6709(value->value))
                    return location;
            }
            return std::nullopt;
        }
    }

    std::optional<GeoLocation> ReadLocation(Path const& path, bool photo)
    {
        if (photo)
        {
            auto extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            if (extension == L".jpg" || extension == L".jpeg")
            {
                if (auto exif = ReadJpegExif(path); exif && exif->location)
                    return exif->location;
            }
        }

        try
        {
            auto input = OpenInput(path);
            if (auto location = LocationFromDictionary(input->metadata))
                return location;
            for (unsigned int index = 0; index < input->nb_streams; ++index)
            {
                if (auto location = LocationFromDictionary(input->streams[index]->metadata))
                    return location;
            }
        }
        catch (std::exception const&)
        {
            return std::nullopt;
        }
        return std::nullopt;
    }

    void ValidateAudio(Path const& path)
    {
        AudioLogCapture logCapture;
        auto file = Utf8(path);
        auto input = OpenInput(path);
        auto audioIndex = FindStream(input.get(), AVMEDIA_TYPE_AUDIO);
        if (audioIndex < 0)
            throw std::runtime_error("The selected music file has no audio stream: " + file);

        auto stream = input->streams[audioIndex];
        auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder)
            throw std::runtime_error("FFmpeg has no decoder for the selected music file: " + file);

        Codec context(avcodec_alloc_context3(decoder));
        if (!context)
            throw std::bad_alloc();
        Check(avcodec_parameters_to_context(context.get(), stream->codecpar),
            "Could not read the music audio format");
        context->err_recognition = AV_EF_EXPLODE;
        Check(avcodec_open2(context.get(), decoder, nullptr),
            "Could not open the music audio decoder");

        Packet packet(av_packet_alloc());
        Frame frame(av_frame_alloc());
        if (!packet || !frame)
            throw std::bad_alloc();
        auto decodeAvailableFrames = [&]()
        {
            for (;;)
            {
                auto result = avcodec_receive_frame(context.get(), frame.get());
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                    return result;
                if (result < 0)
                    ThrowAvError(result, "The selected music file contains invalid audio data: " + file);
                if ((frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                    frame->decode_error_flags != 0)
                    throw std::runtime_error(
                        "FFmpeg detected corrupt audio in the selected music file: " + file);
                av_frame_unref(frame.get());
            }
        };

        for (;;)
        {
            auto result = av_read_frame(input.get(), packet.get());
            if (result == AVERROR_EOF)
                break;
            Check(result, "Could not read the selected music file: " + file);
            if (packet->stream_index == audioIndex)
            {
                result = avcodec_send_packet(context.get(), packet.get());
                if (result < 0)
                    ThrowAvError(result, "The selected music file contains invalid audio data: " + file);
                decodeAvailableFrames();
            }
            av_packet_unref(packet.get());
        }

        Check(avcodec_send_packet(context.get(), nullptr),
            "Could not finish decoding the selected music file: " + file);
        auto result = decodeAvailableFrames();
        if (result != AVERROR_EOF)
            throw std::runtime_error("FFmpeg could not finish decoding the selected music file: " + file);
        auto errors = logCapture.Errors();
        if (!errors.empty())
            throw std::runtime_error(
                "FFmpeg reported errors while decoding the selected music file: " +
                file + "\n" + errors);
    }

    void RenderSegment(
        MediaItem const& item,
        PipelineSettings const& settings,
        Path const& target,
        std::stop_token cancellation)
    {
        CheckCancelled(cancellation);
        std::optional<Path> mapPath;
        if (settings.showMap && item.location)
            mapPath = MapThumbnail::Render(
                *item.location, settings.mapCacheDirectory,
                settings.mapWidth, settings.mapHeight, settings.mapZoom);
        std::optional<Path> cityLabelPath;
        std::optional<TemporaryDirectoryCleanup> cityLabelCleanup;
        if (item.location)
        {
            auto cityName = CityLookup::NearestCity(*item.location);
            auto cityLabelDirectory = target.parent_path() /
                (target.stem().wstring() + L"_city_label");
            cityLabelCleanup.emplace(cityLabelDirectory);
            cityLabelPath = MapThumbnail::RenderCityLabel(
                cityName, cityLabelDirectory,
                settings.width, settings.height);
        }
        auto graph = CreateGraph();
        auto orientation = OrientationFilter(item.file, item.photo);
        auto videoSource = CreateMediaSource(
            graph.get(), "movie", "video_source", item.file);
        auto silenceSource = CreateSource(
            graph.get(), "anullsrc", "silence", "r=44100:cl=stereo");
        AVFilterContext* audioSource{};
        if (item.hasAudio)
            audioSource = CreateMediaSource(
                graph.get(), "amovie", "audio_source", item.file);
        AVFilterContext* mapSource{};
        if (mapPath)
            mapSource = CreateMediaSource(graph.get(), "movie", "map_source", *mapPath);
        AVFilterContext* citySource{};
        if (cityLabelPath)
            citySource = CreateMediaSource(graph.get(), "movie", "city_source", *cityLabelPath);

        for (auto filter : {
             "fps", "scale", "pad", "setsar", "format", "trim", "setpts",
             "atrim", "asetpts", "aformat", "asetnsamples" })
        RequireFilter(filter);
        if (item.photo || mapSource || citySource)
        RequireFilter("loop");
        if (mapSource || citySource)
        RequireFilter("overlay");
        if (item.hasAudio)
        {
        RequireFilter("volume");
        RequireFilter("amix");
        }
        if (orientation.find("transpose") != std::string::npos)
        RequireFilter("transpose");
        if (orientation.find("hflip") != std::string::npos)
        RequireFilter("hflip");
        if (orientation.find("vflip") != std::string::npos)
        RequireFilter("vflip");

        auto videoSink = CreateSink(graph.get(), "buffersink", "video_sink");
        auto audioSink = CreateSink(graph.get(), "abuffersink", "audio_sink");
        std::vector<std::pair<std::string, AVFilterContext*>> sources{
            { "video", videoSource }, { "silence", silenceSource }
        };
        if (audioSource)
            sources.emplace_back("source", audioSource);
        if (mapSource)
            sources.emplace_back("map", mapSource);
        if (citySource)
            sources.emplace_back("city", citySource);
        ParseFilterGraph(
            graph.get(),
            VideoFilterDescription(
                settings, item.duration, item.photo, orientation,
                mapSource != nullptr, citySource != nullptr) + ";" +
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
                Check(av_interleaved_write_frame(output.get(), packet.get()),
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
            "asetpts=PTS-STARTPTS,asetnsamples=n=1024:p=1[audio_out]";
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
