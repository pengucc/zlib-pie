/* pinflate.h -- zlib-pie
 * Copyright (C) 2019 Peng-Hsien Lai

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

 */

#ifndef  __PINFLATE_H__
#define  __PINFLATE_H__

#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>

#define PIE_SCROLL_BUFFER_THRESHOLD 200000
#define PIE_DECODE_BUFFER_THRESHOLD 200000
#define PIE_LENG_BUFFER_SIZE        200000
#define PIE_LENG_BUFFER_REDZONE     100000
#define PIE_TOKEN_SCROLL_BUFFER -3
#define PIE_TOKEN_END     -4
#define PIE_TOKEN_COPY_BLOCK      -11
#define PIE_TOKEN_ROTATE_BUFFER   -12
#define PIE_TOKEN_REWIND   -13
#define PIE_TOKEN_TRAP       -16

struct gz_pinflate{

    int assemble_done;

    pthread_cond_t  cond;
    sem_t           gz_sem;   // the main thread has seen a gz header?

    unsigned char* curr_buff;
    unsigned char  eof;
    unsigned long  crc32;

    unsigned char* prev_buff_returned;
    unsigned char* prev_buff_end;

    pthread_t       thread;             // deflate_mt thread

    unsigned char*  in_buffer[3];
    pthread_mutex_t raw_input_lock;

    signed char*    leng_buffer[3];
    unsigned char*  lite_buffer[3];
    unsigned short* dist_buffer[3];

    int             wbuf_rotate;
    signed char*    next_leng_wpos;
    unsigned char*  next_lite_wpos;
    unsigned short* next_dist_wpos;
    signed char*    leng_wpos_stop;

    int             rbuf_rotate;
    signed char*    next_leng_rpos;
    unsigned char*  next_lite_rpos;
    unsigned short* next_dist_rpos;

    unsigned int    check;
    unsigned int    length;
    unsigned long decoded_len;

    int has_copy_blocks;
    int rewind;

    pthread_mutex_t leng_buffer_lock[3];
};

void pie_next_wbuf(struct gz_pinflate*);
void pie_next_rbuf(struct gz_pinflate*);

#ifdef GZ_READ
void pie_close(gz_statep state);
int gz_decomp_mt(gz_statep);
struct gz_pinflate* pie_init(gz_statep state);
#endif

#endif
