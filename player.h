#ifndef _PLAYER_H
#define _PLAYER_H

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <SDL2/SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000 //channels(2) * data_size(2) * sample_rate(48000)

#define MAX_AUDIO_QUEUE_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_QUEUE_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01 // 同步最小阈值
#define AV_NO_SYNC_THRESHOLD 10.0 // 不同步阈值

#define FF_REFRESH_EVENT (SDL_USEREVENT) //刷新视频显示事件
#define FF_QUIT_EVENT (SDL_USEREVENT + 1) //退出事件

#define VIDEO_FRAME_QUEUE_SIZE 1


//AVPacket数据包队列
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets; //队列中packet的数量
    int size; //队列所占内存空间大小
    SDL_mutex *mutex; //操作队列的同步互斥量
    SDL_cond *cond; //操作队列的同步条件变量
} PacketQueue;

//视频帧结构
typedef struct VideoFrame{
    AVFrame *frame;
    int width, height; /* source height & width */
    double pts;
}VideoFrame;

//SDL2视频结构
typedef struct SDLVideo{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Rect rect;
    int width;
    int height;
    int is_resize;
    SDL_DisplayMode displayMode;
    SDL_mutex *video_mutex; //视频显示互斥量
}SDLVideo;



typedef struct VideoState{
    char filename[1024];
    AVFormatContext *pFormatCtx;
    int audio_index;
    int video_index;

    double audio_clock; //音频播放时钟
    double frame_timer; //记录最后一帧播放的时刻
    double frame_last_pts;
    double frame_last_delay;

    double video_clock; //视频播放时钟
    double video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
    int64_t video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts

    //audio
    AVStream *audioStream;
    AVCodecContext *pACodecCtx;
    PacketQueue audioQueue;
    uint8_t audio_buffer[(MAX_AUDIO_FRAME_SIZE*3)/2];
    unsigned int audio_buffer_size;
    unsigned int audio_buffer_index;
    AVFrame audioFrame;

    AVPacket audioPkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    struct SwrContext *audioSwrCtx;

    //video
    AVStream *videoStream;
    AVCodecContext *pVCodecCtx;
    PacketQueue videoQueue;

    VideoFrame video_frame_buffer[VIDEO_FRAME_QUEUE_SIZE];
    int video_frame_size;
    int video_frame_rindex; //读位置
    int video_frame_windex; //写位置
    SDL_mutex *video_frame_mutex;
    SDL_cond *video_frame_cond;

    SDL_Thread *demuxTid; //解复用线程
    SDL_Thread *videoDecodeTid; //视频解码线程

    struct SwsContext *videoSwsCtx;
    AVFrame *pFrameYUV;

    SDLVideo sdlVideo;

    int pause;
    int quit; //是否退出标记

}VideoState;

int open_player(const char* file);

#endif //_PLAYER_H
