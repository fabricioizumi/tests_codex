#include <iostream>
#include <string>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>
}

static const char* codec_name(AVCodecID id) {
    const AVCodec* decoder = avcodec_find_decoder(id);
    return (decoder && decoder->name) ? decoder->name : "desconhecido";
}

class ScopedSDL {
public:
    ScopedSDL() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
            throw std::runtime_error(std::string("SDL_Init falhou: ") + SDL_GetError());
        }
    }
    ~ScopedSDL() { SDL_Quit(); }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Uso: " << argv[0] << " <arquivo.mpg>\n";
        return 1;
    }

    const char* input = argv[1];

    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, input, nullptr, nullptr) < 0) {
        std::cerr << "Erro ao abrir arquivo: " << input << "\n";
        return 1;
    }

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "Erro ao ler informacoes do stream\n";
        avformat_close_input(&format_ctx);
        return 1;
    }

    int video_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        std::cerr << "Nenhum stream de video encontrado\n";
        avformat_close_input(&format_ctx);
        return 1;
    }

    AVStream* video_stream = format_ctx->streams[video_stream_index];
    const AVCodecParameters* codecpar = video_stream->codecpar;
    if (codecpar->codec_id != AV_CODEC_ID_MPEG1VIDEO) {
        std::cerr << "Aviso: o codec nao e MPEG1 (detectado: "
                  << codec_name(codecpar->codec_id)
                  << "). Tentando decodificar mesmo assim.\n";
    }

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Decoder nao encontrado para " << codec_name(codecpar->codec_id) << "\n";
        avformat_close_input(&format_ctx);
        return 1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Falha ao alocar AVCodecContext\n";
        avformat_close_input(&format_ctx);
        return 1;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        std::cerr << "Falha ao copiar parametros do codec\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 1;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Falha ao abrir codec\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 1;
    }

    try {
        ScopedSDL sdl;

        SDL_Window* window = SDL_CreateWindow(
            "Player MPEG1 (libav + SDL2)",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            codec_ctx->width,
            codec_ctx->height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
        );

        if (!window) {
            throw std::runtime_error(std::string("SDL_CreateWindow falhou: ") + SDL_GetError());
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            SDL_DestroyWindow(window);
            throw std::runtime_error(std::string("SDL_CreateRenderer falhou: ") + SDL_GetError());
        }

        SDL_Texture* texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            codec_ctx->width,
            codec_ctx->height
        );
        if (!texture) {
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            throw std::runtime_error(std::string("SDL_CreateTexture falhou: ") + SDL_GetError());
        }

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* yuv_frame = av_frame_alloc();

        if (!packet || !frame || !yuv_frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (yuv_frame) av_frame_free(&yuv_frame);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            throw std::runtime_error("Falha ao alocar estruturas AVPacket/AVFrame");
        }

        const int yuv_buffer_size = av_image_get_buffer_size(
            AV_PIX_FMT_YUV420P,
            codec_ctx->width,
            codec_ctx->height,
            1
        );

        uint8_t* yuv_buffer = static_cast<uint8_t*>(av_malloc(yuv_buffer_size));
        if (!yuv_buffer) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&yuv_frame);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            throw std::runtime_error("Falha ao alocar buffer YUV");
        }

        if (av_image_fill_arrays(
                yuv_frame->data,
                yuv_frame->linesize,
                yuv_buffer,
                AV_PIX_FMT_YUV420P,
                codec_ctx->width,
                codec_ctx->height,
                1
            ) < 0) {
            av_free(yuv_buffer);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&yuv_frame);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            throw std::runtime_error("Falha ao preencher arrays YUV");
        }

        SwsContext* sws_ctx = sws_getContext(
            codec_ctx->width,
            codec_ctx->height,
            codec_ctx->pix_fmt,
            codec_ctx->width,
            codec_ctx->height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr
        );

        if (!sws_ctx) {
            av_free(yuv_buffer);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&yuv_frame);
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            throw std::runtime_error("Falha ao criar SwsContext");
        }

        bool running = true;
        SDL_Event event;

        auto process_events = [&]() {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    running = false;
                }
            }
        };

        while (running && av_read_frame(format_ctx, packet) >= 0) {
            process_events();
            if (!running) break;

            if (packet->stream_index != video_stream_index) {
                av_packet_unref(packet);
                continue;
            }

            if (avcodec_send_packet(codec_ctx, packet) < 0) {
                av_packet_unref(packet);
                break;
            }

            av_packet_unref(packet);

            while (running) {
                int ret = avcodec_receive_frame(codec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    running = false;
                    break;
                }

                sws_scale(
                    sws_ctx,
                    frame->data,
                    frame->linesize,
                    0,
                    codec_ctx->height,
                    yuv_frame->data,
                    yuv_frame->linesize
                );

                SDL_UpdateYUVTexture(
                    texture,
                    nullptr,
                    yuv_frame->data[0], yuv_frame->linesize[0],
                    yuv_frame->data[1], yuv_frame->linesize[1],
                    yuv_frame->data[2], yuv_frame->linesize[2]
                );

                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);

                process_events();
                SDL_Delay(12);
            }
        }

        avcodec_send_packet(codec_ctx, nullptr);
        while (running) {
            int ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
            if (ret < 0) break;

            sws_scale(
                sws_ctx,
                frame->data,
                frame->linesize,
                0,
                codec_ctx->height,
                yuv_frame->data,
                yuv_frame->linesize
            );

            SDL_UpdateYUVTexture(
                texture,
                nullptr,
                yuv_frame->data[0], yuv_frame->linesize[0],
                yuv_frame->data[1], yuv_frame->linesize[1],
                yuv_frame->data[2], yuv_frame->linesize[2]
            );
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
            process_events();
            SDL_Delay(12);
        }

        sws_freeContext(sws_ctx);
        av_free(yuv_buffer);
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&yuv_frame);

        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        return 1;
    }

    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    return 0;
}
