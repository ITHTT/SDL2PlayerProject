#include "player.h"

/**
 * ��ʼ������
 * @param queue
 */
void packet_queue_init(PacketQueue *queue){
    memset(queue,0, sizeof(queue));

    queue->mutex=SDL_CreateMutex();
    queue->cond=SDL_CreateCond();
}

/**
 * ����������pkt
 * @param queue
 * @param pkt
 * @return
 */
int packet_queue_put(PacketQueue *queue,AVPacket *pkt){
    AVPacketList *pktList;

    //ȷ���������ݰ������������ݱ�����Ϊ���ü�����
    if(av_packet_make_refcounted(pkt)<0){
        return -1;
    }
    pktList=av_malloc(sizeof(AVPacketList));
    if(!pktList){
        return -1;
    }

    pktList->pkt=*pkt;
    pktList->next=NULL;

    SDL_LockMutex(queue->mutex);//����

    if(!queue->last_pkt) //����Ϊ��
        queue->first_pkt=pktList;
    else
        queue->last_pkt->next=pktList;

    queue->last_pkt=pktList;
    queue->nb_packets++;
    queue->size+=pktList->pkt.size;
    // ���������������źţ������ȴ�q->cond����������һ���߳�
    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);

    return 0;
}

/**
 * ������ͷ������
 * @param queue
 * @param pkt
 * @param block
 * @return
 */
int packet_queue_pop(PacketQueue *queue,AVPacket *pkt,int block,int quit){
    AVPacketList *pktList;
    int ret;

    SDL_LockMutex(queue->mutex);//����

    while (1){

        if (quit) {
            ret = -1;
            break;
        }

        pktList=queue->first_pkt;
        if(pktList){
            queue->first_pkt=pktList->next;
            if(!queue->first_pkt)
                queue->last_pkt=NULL;
            queue->nb_packets--;
            queue->size-=pktList->pkt.size;
            *pkt=pktList->pkt;
            av_free(pktList);
            ret=1;
            break;
        }else if(!block){
            // ���п���������־��Ч���������˳�
            ret=0;
            break;
        }else{
            // ���п���������־��Ч����ȴ�
            SDL_CondWait(queue->cond,queue->mutex);
        }
    }
    SDL_UnlockMutex(queue->mutex);
    return ret;
}

/**
 * ��ʱ���ص�����������FF_REFRESH_EVENT�¼���������ʾ��Ƶ֡
 * @param interval
 * @param opaque
 * @return
 */
static Uint32 sdl_refresh_timer_callback(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}

/**
 * ���ö�ʱ��
 * @param is
 * @param delay
 */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_callback, is);
}

/**
 * ��ȡ��Ƶ����ʱ��-�ӿ�ʼ����ǰ��ʱ��
 * @param is
 * @return
 */
double get_audio_clock(VideoState *is) {
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    //��һ����ȡ��PTS
    pts = is->audio_clock;
    // ��Ƶ��������û�в��ŵ�����
    hw_buf_size = is->audio_buffer_size - is->audio_buffer_index;
    // ÿ������Ƶ���ŵ��ֽ���
    bytes_per_sec = 0;
    n = is->pACodecCtx->channels * 2;
    if (is->audioStream) {
        //ÿ������Ƶ���ŵ��ֽ��� => ������ * ͨ���� * ����λ�� (һ��sampleռ�õ��ֽ���)
        bytes_per_sec = is->pACodecCtx->sample_rate * n;
    }
    if (bytes_per_sec) {
        pts -= (double) hw_buf_size / bytes_per_sec;
    }
    return pts;
}

/**
 * ��Ƶ����
 * @param is
 * @param audio_buf
 * @param buf_size
 * @param pts_ptr
 * @return
 */
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
    int len1, data_size = 0;
    AVPacket *pkt = &is->audioPkt;
    double pts;
    int n;

    for (;;) {
        while (is->audio_pkt_size > 0) {
            avcodec_send_packet(is->pACodecCtx, pkt);
            while (avcodec_receive_frame(is->pACodecCtx, &is->audioFrame) == 0) {
                len1 = is->audioFrame.pkt_size;

                if (len1 < 0) {
                    /* if error, skip frame */
                    is->audio_pkt_size = 0;
                    break;
                }

                data_size = 2 * is->audioFrame.nb_samples * 2;
                assert(data_size <= buf_size);

                swr_convert(is->audioSwrCtx,
                            &audio_buf,
                            MAX_AUDIO_FRAME_SIZE * 3 / 2,
                            (const uint8_t **) is->audioFrame.data,
                            is->audioFrame.nb_samples);

            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            if (data_size <= 0) {
                /* No data yet, get more frames */
                continue;
            }
            pts = is->audio_clock;
            *pts_ptr = pts;
            n = 2 * is->pACodecCtx->channels;
            is->audio_clock += (double) data_size /
                               (double) (n * is->pACodecCtx->sample_rate);
            /* We have data, return it and come back for more later */
            return data_size;
        }
        if (pkt->data)
            av_packet_unref(pkt);

        if (is->quit) {
            return -1;
        }
        /* next packet */
        if (packet_queue_pop(&is->audioQueue, pkt, 1,is->quit) < 0) {
            return -1;
        }
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
        /* if update, update the audio clock w/pts */
        if (pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audioStream->time_base) * pkt->pts;
        }
    }
}

/**
 * SDL2������Ƶ�ص�
 * @param userdata
 * @param stream
 * @param len
 */
void audio_callback(void *userdata, Uint8 *stream, int len) {
    VideoState *is = (VideoState *) userdata;
    int len1, audio_size;
    double pts;

    SDL_memset(stream, 0, len);

    while (len > 0&&!is->pause) {
        if (is->audio_buffer_index >= is->audio_buffer_size) {
            // ��Ƶ����
            audio_size = audio_decode_frame(is, is->audio_buffer, sizeof(is->audio_buffer), &pts);
            if (audio_size < 0) {
                // ��Ƶ������󣬲��ž���
                is->audio_buffer_size = 1024 * 2 * 2;
                memset(is->audio_buffer, 0, is->audio_buffer_size);
            } else {
                is->audio_buffer_size = audio_size;
            }
            is->audio_buffer_index = 0;
        }
        //δ���ŵ�����
        len1 = is->audio_buffer_size - is->audio_buffer_index;
        if (len1 > len)
            len1 = len;
        SDL_MixAudio(stream, (uint8_t *) is->audio_buffer + is->audio_buffer_index, len1, SDL_MIX_MAXVOLUME);
        len -= len1;
        stream += len1;
        is->audio_buffer_index += len1;
    }
}

/**
 * ��ȡ��Ƶͬ��ʱ��
 * @param is
 * @param src_frame
 * @param pts
 * @return
 */
double video_synchronize(VideoState *is, AVFrame *src_frame, double pts) {
    double frame_delay; //֡�ӳ�

    if(pts!=0){
        is->video_clock=pts;
    }else{
        pts=is->video_clock;
    }

    //������Ƶ����ʱ��
    frame_delay=av_q2d(is->pVCodecCtx->time_base);
    //���ظ�һ֡������Ӧ�ص���ʱ��
    frame_delay+=src_frame->repeat_pict*(frame_delay*0.5);
    is->video_clock+=frame_delay;
    return pts;
}

/**
 * �����������Ƶ֡
 * @param is
 * @param pFrame
 * @param pts
 * @return
 */
int video_frame_add(VideoState *is,AVFrame *pFrame,double pts){
    VideoFrame *vp;

    SDL_LockMutex(is->video_frame_mutex);
    // wait until we have space for a new VideoFrame
    while (is->video_frame_size>=VIDEO_FRAME_QUEUE_SIZE&&!is->quit){
        SDL_CondWait(is->video_frame_cond,is->video_frame_mutex);
    }
    SDL_UnlockMutex(is->video_frame_mutex);

    if(is->quit)
        return -1;
    //��д��λ�û�ȡVideoFrame
    vp=&is->video_frame_buffer[is->video_frame_windex];

    if(!vp->frame
    ||vp->width!=is->pVCodecCtx->width
    ||vp->height!=is->pVCodecCtx->height){
        vp->frame=av_frame_alloc();
        if(is->quit){
            return -1;
        }
    }

    if(vp->frame){
        vp->pts=pts;
        vp->frame=pFrame;
        if(++is->video_frame_windex==VIDEO_FRAME_QUEUE_SIZE){
            is->video_frame_windex=0;
        }

        SDL_LockMutex(is->video_frame_mutex);
        is->video_frame_size++;
        SDL_UnlockMutex(is->video_frame_mutex);
    }
    return 0;
}

/**
 * ��Ƶ�����߳�
 * @param arg
 * @return
 */
int video_decode_thread(void *arg) {
    VideoState* is=arg;
    AVPacket pkt,*packet=&pkt;
    AVFrame *pFrame;
    double pts;

    pFrame=av_frame_alloc();

    while (1){
        if(is->pause){
            continue;
        }
        if(packet_queue_pop(&is->videoQueue,packet,1,is->quit)<0){
            break;
        }

        //������Ƶ֡
        avcodec_send_packet(is->pVCodecCtx,packet);
        while (avcodec_receive_frame(is->pVCodecCtx,pFrame)==0){
            pts=pFrame->best_effort_timestamp;
            if(pts==AV_NOPTS_VALUE){
                pts=0;
            }
            pts*=av_q2d(is->videoStream->time_base);
            //ͬ����Ƶ
            pts=video_synchronize(is,pFrame,pts);
            //���������Ƶ֡���뵽������
            if(video_frame_add(is,pFrame,pts)<0){
                break;
            }
            av_packet_unref(packet);
        }
    }
    av_frame_free(&pFrame);
    return 0;
}

/**
 * ��Ⱦ��Ƶ
 * @param is
 */
void video_display(VideoState *is) {
    VideoFrame *vp;

    if(is->pause) return;

    if (is->sdlVideo.window && is->sdlVideo.is_resize) {
        SDL_SetWindowSize(is->sdlVideo.window, is->sdlVideo.width, is->sdlVideo.height);
        SDL_SetWindowPosition(is->sdlVideo.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(is->sdlVideo.window);

        //create texture for render
        is->sdlVideo.texture = SDL_CreateTexture(is->sdlVideo.renderer,
                              SDL_PIXELFORMAT_IYUV,
                              SDL_TEXTUREACCESS_STREAMING,
                                    is->sdlVideo.width,
                                    is->sdlVideo.height);

        is->sdlVideo.is_resize=0;
    }

    vp = &is->video_frame_buffer[is->video_frame_rindex];

    // ��Ⱦ����
    if (vp->frame) {
        //ͼ��ת��
        sws_scale(is->videoSwsCtx,
                  (const uint8_t *const *) vp->frame->data,
                  vp->frame->linesize,
                  0,
                  is->pVCodecCtx->height,
                  is->pFrameYUV->data,
                  is->pFrameYUV->linesize);
        SDL_UpdateYUVTexture(is->sdlVideo.texture, NULL,
                             is->pFrameYUV->data[0], is->pFrameYUV->linesize[0],
                             is->pFrameYUV->data[1], is->pFrameYUV->linesize[1],
                             is->pFrameYUV->data[2], is->pFrameYUV->linesize[2]);

        is->sdlVideo.rect.x = 0;
        is->sdlVideo.rect.y = 0;
        is->sdlVideo.rect.w = is->sdlVideo.width;
        is->sdlVideo.rect.h = is->sdlVideo.height;
        SDL_LockMutex(is->sdlVideo.video_mutex);
        SDL_RenderClear(is->sdlVideo.renderer);
        SDL_RenderCopy(is->sdlVideo.renderer, is->sdlVideo.texture, NULL, &is->sdlVideo.rect);
        SDL_RenderPresent(is->sdlVideo.renderer);
        SDL_UnlockMutex(is->sdlVideo.video_mutex);
    }
}

/**
 * ��Ƶˢ�²��ţ���Ԥ����һ֡�Ĳ���ʱ�䣬�����µĶ�ʱ��
 * @param user_data
 */
void video_refresh_timer(void *user_data) {
    VideoState *is = (VideoState *) user_data;
    VideoFrame *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;

    if (is->videoStream&&!is->pause) {
        if (is->video_frame_size == 0) {
            schedule_refresh(is, 1);
        } else {
            // ��������ȡ��һ֡��Ƶ֡
            vp = &is->video_frame_buffer[is->video_frame_rindex];

            is->video_current_pts = vp->pts;
            is->video_current_pts_time = av_gettime();
            printf("video_current_pst��%f,video_current_pts_time��%ld\n",is->video_current_pts,is->video_current_pts_time);
            // ��ǰFrameʱ���ȥ��һ֡��ʱ�䣬��ȡ��֡���ʱ��
            delay = vp->pts - is->frame_last_pts;
            if (delay <= 0 || delay >= 1.0) {
                // ��ʱС��0�����1�루̫�������Ǵ���ģ�����ʱʱ������Ϊ��һ�ε���ʱʱ��
                delay = is->frame_last_delay;
            }
            // ������ʱ��PTS���ȴ��´�ʹ��
            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;

            // ��ȡ��ƵAudio_Clock
            ref_clock = get_audio_clock(is);
            // �õ���ǰPTS��Audio_Clock�Ĳ�ֵ
            diff = vp->pts - ref_clock;

            /* Skip or repeat the frame. Take delay into account
               FFPlay still doesn't "know if this is the best guess." */
            sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
            if (fabs(diff) < AV_NO_SYNC_THRESHOLD) {
                if (diff <= -sync_threshold) {
                    delay = 0;
                } else if (diff >= sync_threshold) {
                    delay = 2 * delay;
                }
            }
            is->frame_timer += delay;
            // ��������Ҫ��ʱ��ʱ��
            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            if (actual_delay < AV_SYNC_THRESHOLD) {
                // ��ʱʱ���С��������Сֵ
                actual_delay = AV_SYNC_THRESHOLD;
            }
            // ������ʱʱ���������ö�ʱ����ˢ����Ƶ
            schedule_refresh(is, (int) (actual_delay * 1000 + 0.5));

            // ��Ƶ֡��ʾ
            video_display(is);

            // ������Ƶ֡�����±�
            if (++is->video_frame_rindex == VIDEO_FRAME_QUEUE_SIZE) {
                is->video_frame_rindex = 0;
            }
            SDL_LockMutex(is->video_frame_mutex);
            // ��Ƶ֡�����һ
            is->video_frame_size--;
            SDL_CondSignal(is->video_frame_cond);
            SDL_UnlockMutex(is->video_frame_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

/**
 * ������׼������
 * @param is
 * @param stream_index
 * @return
 */
int open_media_stream(VideoState *is,int stream_index){
    AVFormatContext *pFormatCtx=is->pFormatCtx;
    AVCodecContext *pCodecCtx=NULL;
    AVCodec *pCodec=NULL;
    int ret;

    //�ж����������Ƿ���ȷ
    if(stream_index<0||stream_index>=pFormatCtx->nb_streams){
        return -1;
    }

    //��ʼ��pCodecCtx
    pCodecCtx=avcodec_alloc_context3(NULL);
    ret=avcodec_parameters_to_context(pCodecCtx,pFormatCtx->streams[stream_index]->codecpar);
    if(ret<0){
        return -1;
    }

    //���ҽ�����pCodec
    pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
    if(!pCodec){
        return -1;
    }

    //�򿪽�����
    ret=avcodec_open2(pCodecCtx,pCodec,NULL);
    if(ret<0){
        return -1;
    }

    SDL_AudioSpec wanted_spec;
    //������Ƶ����Ƶ������Ϊ������׼��
    switch (pCodecCtx->codec_type){
        case AVMEDIA_TYPE_AUDIO:
            //��Ƶ
            wanted_spec.freq=pCodecCtx->sample_rate;
            wanted_spec.format=AUDIO_S16SYS;
            wanted_spec.channels=pCodecCtx->channels;
            wanted_spec.silence=0;
            wanted_spec.samples=SDL_AUDIO_BUFFER_SIZE;
            wanted_spec.callback=audio_callback;
            wanted_spec.userdata=is;

            printf("wanted spec: channels:%d, sample_fmt:%d, sample_rate:%d \n",
                    2, AUDIO_S16SYS, pCodecCtx->sample_rate);

            //����Ƶ�豸
            if(SDL_OpenAudio(&wanted_spec,NULL)<0){
                printf("����Ƶ�豸����%s\n",SDL_GetError());
                return -1;
            }

            is->audio_index=stream_index;
            is->audioStream=pFormatCtx->streams[stream_index];
            is->pACodecCtx=pCodecCtx;
            is->audio_buffer_size=0;
            is->audio_buffer_index=0;
            memset(&is->audioPkt,0, sizeof(AVPacket));
            //��ʼ����Ƶ����
            packet_queue_init(&is->audioQueue);

            //Out Audio Param
            uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;

            //int out_nb_samples = is->pACodecCtx->frame_size;
            int out_sample_rate = is->pACodecCtx->sample_rate;
            //int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);

            int64_t in_channel_layout = av_get_default_channel_layout(is->pACodecCtx->channels);

            // ��Ƶ�ز���
            struct SwrContext *audio_convert_ctx;
            audio_convert_ctx = swr_alloc();
            swr_alloc_set_opts(audio_convert_ctx,
                               out_channel_layout,
                               AV_SAMPLE_FMT_S16,
                               out_sample_rate,
                               in_channel_layout,
                               is->pACodecCtx->sample_fmt,
                               is->pACodecCtx->sample_rate,
                               0,
                               NULL);

            swr_init(audio_convert_ctx);
            is->audioSwrCtx = audio_convert_ctx;

            // ��ʼ������Ƶ��audio_callback�ص�
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            //��Ƶ
            is->video_index=stream_index;
            is->videoStream=pFormatCtx->streams[stream_index];
            is->pVCodecCtx=pCodecCtx;

            is->frame_timer=av_gettime()/1000000.0;
            is->frame_last_delay=40e-3;
            is->video_current_pts_time=av_gettime();

            packet_queue_init(&is->videoQueue);

            //������Ƶ�����߳�
            is->videoDecodeTid=SDL_CreateThread(video_decode_thread,"video_decode_thread",is);
            break;
        default:
            break;
    }
}

// �⸴�ã���ȡ��Ƶ����Ƶ��������packet���������
int demux_thread(void *arg) {

    int err_code;
    char errors[1024] = {0};

    int w, h;

    VideoState *is = (VideoState *) arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    is->video_index = -1;
    is->audio_index = -1;

    /* open input file, and allocate format context */
    if ((err_code = avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)) < 0) {
        av_strerror(err_code, errors, 1024);
        fprintf(stderr, "Could not open source file %s, %d(%s)\n", is->filename, err_code, errors);
        return -1;
    }

    is->pFormatCtx = pFormatCtx;

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    // Find the first video stream
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            video_index < 0) {
            video_index = i;
        }
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            audio_index < 0) {
            audio_index = i;
        }
    }
    if (audio_index >= 0) {
        open_media_stream(is, audio_index);
    }
    if (video_index >= 0) {
        open_media_stream(is, video_index);
    }

    if (is->video_index < 0 || is->audio_index < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }

//    screen_width = is->video_ctx->width;
//    screen_height = is->video_ctx->height;

    is->sdlVideo.width=is->pVCodecCtx->width;
    is->sdlVideo.height=is->pVCodecCtx->height;
    if(is->sdlVideo.width>is->sdlVideo.displayMode.w
    ||is->sdlVideo.height>is->sdlVideo.displayMode.h){
        is->sdlVideo.width=is->sdlVideo.displayMode.w;
        is->sdlVideo.height=is->sdlVideo.displayMode.h;
    }

    AVFrame *pFrameYUV=av_frame_alloc();//��pFrameRawԭʼ֡ת����YUV��ʽ��֡
    //����ΪpFrameYUV����Ƶ���ݻ������Ĵ�С
    int bufferSize=av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                            is->sdlVideo.width,
                                            is->sdlVideo.height,
                                            1);
    //buffer��ΪpFrameYUV����Ƶ������
    uint8_t *buffer=av_malloc(bufferSize);
    av_image_fill_arrays(pFrameYUV->data,
                         pFrameYUV->linesize,
                         buffer,
                         AV_PIX_FMT_YUV420P,
                         is->sdlVideo.width,
                         is->sdlVideo.height,
                         1);

    //���岢��ʼ��SWSContext�ṹ�壬����ͼ��ת��
    struct SwsContext *swsCtx=sws_getContext(
            is->pVCodecCtx->width, //ԭͼ���
            is->pVCodecCtx->height, //ԭͼ���
            is->pVCodecCtx->pix_fmt, //ԭͼ���ʽ
            is->sdlVideo.width, //Ŀ��ͼ���
            is->sdlVideo.height, //Ŀ��ͼ���
            AV_PIX_FMT_YUV420P, //Ŀ��ͼ���ʽ
            SWS_BICUBIC, //flags
            NULL,
            NULL,
            NULL);

    is->pFrameYUV=pFrameYUV;
    is->videoSwsCtx=swsCtx;

    for (;;) {
        if(is->pause){
            SDL_Delay(10);
            continue;
        }
        if (is->quit) {
            break;
        }
        // seek stuff goes here
        if (is->audioQueue.size > MAX_AUDIO_FRAME_SIZE ||
            is->videoQueue.size > MAX_VIDEO_QUEUE_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if (av_read_frame(is->pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if (packet->stream_index == is->video_index) {
            packet_queue_put(&is->videoQueue, packet);
        } else if (packet->stream_index == is->audio_index) {
            packet_queue_put(&is->audioQueue, packet);
        } else {
            av_packet_unref(packet);
        }
    }
    /* all done - wait for it */
    while (!is->quit) {
        SDL_Delay(100);
    }

    fail:
       {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
       }
    return 0;
}

VideoState* player_init(const char *file){
    VideoState *is=NULL;

    //��ʼ������
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    is = av_mallocz(sizeof(VideoState));
    if(!is){
        printf("��ʼ��VideoStateʧ��!\n");
        return NULL;
    }


    strcpy(is->filename,file);
    is->sdlVideo.width=640;
    is->sdlVideo.height=480;

    is->sdlVideo.window=SDL_CreateWindow("Media Player",
                     100,
                     100,
                     640, 480,
                     SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    if (!is->sdlVideo.window) {
        fprintf(stderr, "����SDL2����ʧ�ܣ�%s\n", SDL_GetError());
        goto fail;
    }

    is->sdlVideo.renderer=SDL_CreateRenderer(is->sdlVideo.window,-1,-0);
    if(!is->sdlVideo.renderer){
        fprintf(stderr,"����SDL2������Ⱦ��ʧ�ܣ�%s\n",SDL_GetError());
        goto fail;
    }

    is->sdlVideo.is_resize=1;

    is->sdlVideo.video_mutex=SDL_CreateMutex();

    is->video_frame_mutex=SDL_CreateMutex();
    is->video_frame_cond=SDL_CreateCond();

    SDL_GetDesktopDisplayMode(0,&is->sdlVideo.displayMode);
    printf("screen_width=%d,screen_height=%d\n",is->sdlVideo.displayMode.w,is->sdlVideo.displayMode.h);

    return is;
    fail:
        if(is->video_frame_mutex){
            SDL_DestroyMutex(is->video_frame_mutex);
        }
        if(is->video_frame_cond){
            SDL_DestroyCond(is->video_frame_cond);
        }
        if(is->sdlVideo.video_mutex){
            SDL_DestroyMutex(is->sdlVideo.video_mutex);
        }
        if(is->sdlVideo.window){
            SDL_DestroyWindow(is->sdlVideo.window);
        }
        if(is->sdlVideo.renderer){
            SDL_DestroyRenderer(is->sdlVideo.renderer);
        }
        av_free(is);
        return NULL;
}

int player_destroy(VideoState *is){
    if(is){
        is->quit=1;
        if(is->pFormatCtx){
            avformat_close_input(&is->pFormatCtx);
        }

        if(is->pFrameYUV){
            av_frame_free(&is->pFrameYUV);
        }

        if(is->audioSwrCtx){
            swr_free(&is->audioSwrCtx);
        }

        if(is->pACodecCtx){
            avcodec_free_context(&is->pACodecCtx);
        }

        if(is->pVCodecCtx){
            avcodec_free_context(&is->pVCodecCtx);
        }

        if(is->video_frame_mutex){
            SDL_DestroyMutex(is->video_frame_mutex);
        }
        if(is->video_frame_cond){
            SDL_DestroyCond(is->video_frame_cond);
        }
        if(is->sdlVideo.video_mutex){
            SDL_DestroyMutex(is->sdlVideo.video_mutex);
        }
        if(is->sdlVideo.window){
            SDL_DestroyWindow(is->sdlVideo.window);
        }
        if(is->sdlVideo.renderer){
            SDL_DestroyRenderer(is->sdlVideo.renderer);
        }
        if(is->sdlVideo.texture){
            SDL_DestroyTexture(is->sdlVideo.texture);
        }

        av_free(is);
    }
    return 0;
}

int open_player(const char* file){
    VideoState *is=player_init(file);
    if(!is){
        return -1;
    }
    // ��ʱˢ����
    schedule_refresh(is, 40);

    // �����⸴���߳�
    is->demuxTid = SDL_CreateThread(demux_thread, "demux_thread", is);
    if (!is->demuxTid) {
        player_destroy(is);
        goto Destroy;
    }

    SDL_Event event;
    for (;;) {
        // �ȴ�SDL�¼�����������
        SDL_WaitEvent(&event);
        switch (event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT: // �˳�
                is->quit = 1;
                goto Destroy;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    is->quit = 1;
                    goto Destroy;
                }else if(event.key.keysym.sym==SDLK_SPACE){
                    if(is->pause){
                        //����ͣ״̬��Ϊ����״̬������֡����ʱ��
                        is->frame_timer+=av_gettime()/1000000.0-is->frame_timer;
                    }
                    is->pause=!is->pause;
                    SDL_PauseAudio(is->pause);
                    printf("frame_timer��%f , times��%f ,pause��%d\n",is->frame_timer,av_gettime()/1000000.0,is->pause);
                }
                break;
            case FF_REFRESH_EVENT: // ��ʱ��ˢ���¼�
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }

    Destroy:
       player_destroy(is);
       SDL_Quit();
    return 0;

}

