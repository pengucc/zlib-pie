/* pinflate.c -- zlib-pie
 * Copyright (C) 2019 Peng-Hsien Lai

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

 */

#include "gzguts.h"
#include "pinflate.h"

int gz_decomp_mt(gz_statep state)
{
    z_streamp strm = &(state->strm);
    if( strm->avail_out == 0){
        return Z_OK;
    }

    struct gz_pinflate* pi = strm->pi;

    if(strm->next_out != NULL){
        abort();
    }

    do{
        if(pi->prev_buff_returned < pi->prev_buff_end){
            int length = strm->avail_out;
            int left   = pi->prev_buff_end - pi->prev_buff_returned;
            if(length>left) length = left;
            strm->avail_out -= length;
            state->x.next = pi->prev_buff_returned;
            state->x.have = length;
            pi->prev_buff_returned += length;
            if(length==left && pi->assemble_done){
                state->how = LOOK;
                if(state->eof){
                    pi->eof = 1;
                }else{
                    pi->assemble_done = 0;
                    pi->crc32 = 0;
                }
            }
            return Z_OK;
        }

        if(pi->eof){
            state->x.next = NULL;
            state->x.have = 0;
            return -1;
        }

        signed char*    i_len  = pi->next_leng_rpos;
        unsigned char*  i_lit  = pi->next_lite_rpos;
        unsigned short* i_dist = pi->next_dist_rpos;
        unsigned char*  buf    = pi->curr_buff+32768;
        for(;; ++i_len){
            signed short len = *i_len;
            if(len==0){
                *buf++ = *i_lit++;
            }else{
                int dist = *i_dist++;
                if(!dist) dist = 32768;
                if(len>0){
                    int length = len;
                    unsigned char* src = buf-dist;
                    memcpy(buf, src, length);
                    buf+=length;
                }else if(len>=-2){
                    int length = *i_lit++;
                    if(len==-2){
                        length += 128;
                    }
                    if(dist>=length){
                        memcpy(buf, buf-dist, length);
                        buf += length;
                    }else{
                        while(1){
                            if(dist<length){
                                memcpy(buf, buf-dist, dist);
                                buf+=dist;
                            }else{
                                memcpy(buf, buf-dist, length);
                                buf+=length;
                                break;
                            }
                            length-=dist;
                            dist<<=1;
                        }
                    }

                }else{
                    i_dist--;
                    if(len==PIE_TOKEN_SCROLL_BUFFER){
                        memcpy(pi->curr_buff, buf-32768, 32768);
                        pi->prev_buff_returned = pi->curr_buff+32768;
                        pi->prev_buff_end      = buf;
                        i_len++;
                        break;
                    }else if(len==PIE_TOKEN_END){
                        pi->prev_buff_returned = pi->curr_buff+32768;
                        pi->prev_buff_end      = buf;
                        pi->assemble_done = 1;
                        i_len++;
                        break;
                    }else if(len==PIE_TOKEN_COPY_BLOCK){
                        i_len += sizeof(void*)-1;
                        unsigned long src = 0;
                        for(int i=0; i<sizeof(void*); ++i){
                            src |= (((unsigned long)(*i_lit++))<<(i*8));
                        }
                        int length = *i_dist++;
                        memcpy(buf, (void*)src, length);
                        buf+=length;
                    }else if(len==PIE_TOKEN_ROTATE_BUFFER){
                        pie_next_rbuf(pi);
                        i_len = pi->next_leng_rpos-1;
                        i_lit = pi->next_lite_rpos;
                        i_dist= pi->next_dist_rpos;
                    }else if(len==PIE_TOKEN_TRAP){
                        fprintf(stderr, "Error, bug detected.\n");
                        abort();
                    }else{
                        fprintf(stderr, "Error, unknown token %d\n", len);
                        abort();
                    }
                }
            }
        }
        pi->next_leng_rpos = i_len;
        pi->next_lite_rpos = i_lit;
        pi->next_dist_rpos = i_dist;

        pi->crc32 = crc32(pi->crc32, pi->prev_buff_returned, pi->prev_buff_end - pi->prev_buff_returned);

    }while(1);

    abort();
}

int gz_avail OF((gz_statep));

void* inflate_mt(void* argv){

    gz_statep state = (gz_statep) argv;
    z_streamp strm = &(state->strm);
    struct gz_pinflate* pi = strm->pi;
    pi->wbuf_rotate = 0;
    pthread_mutex_lock(pi->leng_buffer_lock+0);
    pi->next_leng_wpos = pi->leng_buffer[0];
    pi->next_lite_wpos = pi->lite_buffer[0];
    pi->next_dist_wpos = pi->dist_buffer[0];
    pi->leng_wpos_stop = pi->next_leng_wpos + PIE_LENG_BUFFER_SIZE - PIE_LENG_BUFFER_REDZONE;
    pi->rewind = 2;
    pthread_mutex_lock(pi->leng_buffer_lock+1);
    pthread_cond_signal(&pi->cond);
    pthread_mutex_unlock(pi->leng_buffer_lock+1);

    int ret;

    int let_go = 0;
    int cnt = 0;
    while (1) {
        sem_wait(&pi->gz_sem);
        if(pi->rewind==3) goto terminate;
        if(pi->rewind==2){
            fprintf(stderr, "Error, rewind at beginning.\n");
            abort();
        }
        if(strm->avail_in==0 && state->eof){
            break;
        }

        do {
            /* get more input for inflate() */
            if (strm->avail_in == 0){
                pthread_mutex_lock(&pi->raw_input_lock);
                if(pi->rewind){
                    pthread_mutex_unlock(&pi->raw_input_lock);
                    break;
                }
                int stat = gz_avail(state);
                pthread_mutex_unlock(&pi->raw_input_lock);
                if(stat == -1)
                    goto handle_error;
            }
            if (strm->avail_in == 0) {
                gz_error(state, Z_BUF_ERROR, "unexpected end of file");
                goto handle_error;
            }

            /* decompress and handle errors */

            cnt++;
            ret = inflate(strm, Z_NO_FLUSH);

            if (pi->next_leng_wpos >= pi->leng_wpos_stop || ret==Z_STREAM_END || pi->has_copy_blocks){
                if(ret==Z_STREAM_END)
                   *pi->next_leng_wpos++ = PIE_TOKEN_END;
               *pi->next_leng_wpos++ = PIE_TOKEN_ROTATE_BUFFER;
                pie_next_wbuf(pi);
                state->in = pi->in_buffer[pi->wbuf_rotate];
                pi->has_copy_blocks = 0;
                if(strm->avail_in){
                    memcpy(state->in, strm->next_in, strm->avail_in);
                    strm->next_in = state->in;
                }
            }

            if (ret == Z_STREAM_ERROR || ret == Z_NEED_DICT) {
                gz_error(state, Z_STREAM_ERROR,
                         "internal error: inflate stream corrupt");
                goto handle_error;
            }
            if (ret == Z_MEM_ERROR) {
                gz_error(state, Z_MEM_ERROR, "out of memory");
                goto handle_error;
            }
            if (ret == Z_DATA_ERROR) {              /* deflate stream invalid */
                gz_error(state, Z_DATA_ERROR,
                         strm->msg == NULL ? "compressed data error" : strm->msg);
                goto handle_error;
            }
        } while (ret != Z_STREAM_END);

        int rewind = 0;
        pthread_mutex_lock(&pi->raw_input_lock);
        rewind = pi->rewind;
        pi->rewind = 2;
        pthread_mutex_unlock(&pi->raw_input_lock);

        if(rewind==3) goto terminate;

        if(rewind){
            pi->leng_buffer[pi->wbuf_rotate][0] = PIE_TOKEN_REWIND;
            pie_next_wbuf(pi);
        }

    }
terminate:
    return NULL;

handle_error:
    return NULL;
}

void pie_next_wbuf(struct gz_pinflate* pi){
    int next_buffer =  pi->wbuf_rotate + 1;
    if (next_buffer == 3)
        next_buffer =  0 ;
    pthread_mutex_lock  (pi->leng_buffer_lock +     next_buffer);
    pthread_mutex_unlock(pi->leng_buffer_lock + pi->wbuf_rotate);
    pi->wbuf_rotate    = next_buffer;
    pi->next_leng_wpos = pi->leng_buffer[pi->wbuf_rotate];
    pi->next_lite_wpos = pi->lite_buffer[pi->wbuf_rotate];
    pi->next_dist_wpos = pi->dist_buffer[pi->wbuf_rotate];
    pi->leng_wpos_stop = pi->next_leng_wpos + PIE_LENG_BUFFER_SIZE - PIE_LENG_BUFFER_REDZONE;
}

void pie_next_rbuf(struct gz_pinflate* pi){
    int next_buffer =  pi->rbuf_rotate + 1;
    if (next_buffer == 3)
        next_buffer =  0 ;
    pthread_mutex_lock  (pi->leng_buffer_lock +     next_buffer);
    pthread_mutex_unlock(pi->leng_buffer_lock + pi->rbuf_rotate);
    pi->rbuf_rotate    = next_buffer;
    pi->next_leng_rpos = pi->leng_buffer[pi->rbuf_rotate];
    pi->next_lite_rpos = pi->lite_buffer[pi->rbuf_rotate];
    pi->next_dist_rpos = pi->dist_buffer[pi->rbuf_rotate];
}

struct gz_pinflate* pie_init(gz_statep state){
    // init memory resources
    struct gz_pinflate* pi = (struct gz_pinflate*)
      malloc(sizeof(struct gz_pinflate));
    int mem_alloc_ok = 0;
    if(pi){
	mem_alloc_ok = 1;
	memset(pi, 0, sizeof(struct gz_pinflate));
	pi->curr_buff = (unsigned char*) malloc(32768+PIE_SCROLL_BUFFER_THRESHOLD+300);
	if(!pi->curr_buff) mem_alloc_ok = 0;
	for(int i=0; i<3; ++i){
	    pi->in_buffer  [i] = (unsigned char *) malloc(state->want);
	    pi->leng_buffer[i] = (  signed char* ) malloc(PIE_LENG_BUFFER_SIZE*sizeof(  signed char ));
	    pi->lite_buffer[i] = (unsigned char* ) malloc(PIE_LENG_BUFFER_SIZE*sizeof(unsigned char ));
	    pi->dist_buffer[i] = (unsigned short*) malloc(PIE_LENG_BUFFER_SIZE*sizeof(unsigned short));
	    if(!pi->in_buffer[i] || !pi->leng_buffer[i] || !pi->lite_buffer[i] || !pi->dist_buffer[i]){
		mem_alloc_ok = 0;
		break;
	    }
	}
    }
    // init pthread resources
    if(mem_alloc_ok){
	int inited=0;
	if( ++inited && sem_init(&pi->gz_sem, 0, 0)
	 || ++inited && pthread_cond_init(&pi->cond, 0)
	 || ++inited && pthread_mutex_init(pi->leng_buffer_lock+0, 0)
	 || ++inited && pthread_mutex_init(pi->leng_buffer_lock+1, 0)
	 || ++inited && pthread_mutex_init(pi->leng_buffer_lock+2, 0)
	 || ++inited && pthread_mutex_init(&pi->raw_input_lock, 0)
	){
	    // if any of *_init failed, i.e. the return value is not zero
	    if(inited && --inited) sem_destroy(&pi->gz_sem);
	    if(inited && --inited) pthread_cond_destroy(&pi->cond);
	    if(inited && --inited) pthread_mutex_destroy(pi->leng_buffer_lock+0);
	    if(inited && --inited) pthread_mutex_destroy(pi->leng_buffer_lock+1);
	    if(inited && --inited) pthread_mutex_destroy(pi->leng_buffer_lock+2);
	    if(inited && --inited) pthread_mutex_destroy(&pi->raw_input_lock);
	    mem_alloc_ok = 0;
	}
    }
    // start decoder thread
    if(pi && mem_alloc_ok){
	state->strm.pi = pi;
	pi->rbuf_rotate = 2;
	pthread_mutex_lock(pi->leng_buffer_lock+2);
	pi->next_leng_rpos = pi->leng_buffer[2];
	pi->next_lite_rpos = pi->lite_buffer[2];
	pi->next_dist_rpos = pi->dist_buffer[2];
       *pi->next_leng_rpos = PIE_TOKEN_ROTATE_BUFFER;
	pthread_mutex_lock(pi->leng_buffer_lock+1);
	if(pthread_create(&pi->thread, NULL, inflate_mt, state)==0){
	    pthread_cond_wait(&pi->cond, pi->leng_buffer_lock+1);
	    pthread_mutex_unlock(pi->leng_buffer_lock+1);
	}else{
	    mem_alloc_ok = 0;
	}
    }
    // release memory if failed
    if(pi && !mem_alloc_ok){
	free(pi->curr_buff);
	for(int i=0; i<3; ++i){
	    free(pi->in_buffer  [i]);
	    free(pi->leng_buffer[i]);
	    free(pi->lite_buffer[i]);
	    free(pi->dist_buffer[i]);
	}
	free(pi);
	pi = NULL;
    }
    return pi;
}

void pie_close(gz_statep state){
    struct gz_pinflate* pi = state->strm.pi;
    if(!pi) return;
    // terminate the decoder thread
    pthread_mutex_unlock(pi->leng_buffer_lock+pi->rbuf_rotate);
    pthread_mutex_lock  (&pi->raw_input_lock);
    pi->rewind = 3;
    pthread_mutex_unlock(&pi->raw_input_lock);
    sem_post(&pi->gz_sem);
    void* rtn;
    pthread_join(pi->thread, &rtn);
    // destroy pthread resources
    sem_destroy(&pi->gz_sem);
    pthread_cond_destroy(&pi->cond);
    pthread_mutex_destroy(pi->leng_buffer_lock+0);
    pthread_mutex_destroy(pi->leng_buffer_lock+1);
    pthread_mutex_destroy(pi->leng_buffer_lock+2);
    pthread_mutex_destroy(&pi->raw_input_lock);
    // free memory
    free(pi->curr_buff);
    for(int i=0; i<3; ++i){
	free(pi->in_buffer  [i]);
	free(pi->leng_buffer[i]);
	free(pi->lite_buffer[i]);
	free(pi->dist_buffer[i]);
    }
    state->in  = NULL;
    state->out = NULL;
    free(pi);
    state->strm.pi = NULL;
}

