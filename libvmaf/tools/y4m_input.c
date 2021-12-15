/*Daala video codec
Copyright (c) 2002-2007 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#include "vidinput.h"
#include <stdlib.h>
#include <string.h>

typedef struct y4m_input y4m_input;

/*The function used to perform chroma conversion.*/
typedef void (*y4m_convert_func)(y4m_input *_y4m,
 unsigned char *_dst,unsigned char *_aux);

/** Linkage will break without this if using a C++ compiler, and will issue
 * warnings without this for a C compiler*/
#if defined(__cplusplus)
# define OC_EXTERN extern
#else
# define OC_EXTERN
#endif

#define OC_MINI(_a,_b)      ((_a)>(_b)?(_b):(_a))
#define OC_MAXI(_a,_b)      ((_a)<(_b)?(_b):(_a))
#define OC_CLAMPI(_a,_b,_c) (OC_MAXI(_a,OC_MINI(_b,_c)))

struct y4m_input{
  int               frame_w;
  int               frame_h;
  int               pic_w;
  int               pic_h;
  int               pic_x;
  int               pic_y;
  int               fps_n;
  int               fps_d;
  int               par_n;
  int               par_d;
  char              interlace;
  int               src_c_dec_h;
  int               src_c_dec_v;
  int               dst_c_dec_h;
  int               dst_c_dec_v;
  char              chroma_type[16];
  int               depth;
  /*The size of each converted frame buffer.*/
  size_t            dst_buf_sz;
  /*The amount to read directly into the converted frame buffer.*/
  size_t            dst_buf_read_sz;
  /*The size of the auxilliary buffer.*/
  size_t            aux_buf_sz;
  /*The amount to read into the auxilliary buffer.*/
  size_t            aux_buf_read_sz;
  y4m_convert_func  convert;
  unsigned char    *dst_buf;
  unsigned char    *aux_buf;
};

static int y4m_parse_tags(y4m_input *_y4m,char *_tags){
  int   got_w;
  int   got_h;
  int   got_fps;
  int   got_interlace;
  int   got_par;
  int   got_chroma;
  char *p;
  char *q;
  got_w=got_h=got_fps=got_interlace=got_par=got_chroma=0;
  for(p=_tags;;p=q){
    /*Skip any leading spaces.*/
    while(*p==' ')p++;
    /*If that's all we have, stop.*/
    if(p[0]=='\0')break;
    /*Find the end of this tag.*/
    for(q=p+1;*q!='\0'&&*q!=' ';q++);
    /*Process the tag.*/
    switch(p[0]){
      case 'W':{
        if(sscanf(p+1,"%d",&_y4m->pic_w)!=1)return -1;
        got_w=1;
      }break;
      case 'H':{
        if(sscanf(p+1,"%d",&_y4m->pic_h)!=1)return -1;
        got_h=1;
      }break;
      case 'F':{
        if(sscanf(p+1,"%d:%d",&_y4m->fps_n,&_y4m->fps_d)!=2){
          return -1;
        }
        got_fps=1;
      }break;
      case 'I':{
        _y4m->interlace=p[1];
        got_interlace=1;
      }break;
      case 'A':{
        if(sscanf(p+1,"%d:%d",&_y4m->par_n,&_y4m->par_d)!=2){
          return -1;
        }
        got_par=1;
      }break;
      case 'C':{
        if(q-p>16)return -1;
        memcpy(_y4m->chroma_type,p+1,q-p-1);
        _y4m->chroma_type[q-p-1]='\0';
        got_chroma=1;
      }break;
      /*Ignore unknown tags.*/
    }
  }
  if(!got_w||!got_h||!got_fps)return -1;
  if(!got_interlace)_y4m->interlace='?';
  if(!got_par)_y4m->par_n=_y4m->par_d=0;
  /*Chroma-type is not specified in older files, e.g., those generated by
     mplayer.*/
  if(!got_chroma)strcpy(_y4m->chroma_type,"420");
  return 0;
}

/*All anti-aliasing filters in the following conversion functions are based on
   one of two window functions:
  The 6-tap Lanczos window (for down-sampling and shifts):
   sinc(\pi*t)*sinc(\pi*t/3), |t|<3  (sinc(t)==sin(t)/t)
   0,                         |t|>=3
  The 4-tap Mitchell window (for up-sampling):
   7|t|^3-12|t|^2+16/3,             |t|<1
   -(7/3)|x|^3+12|x|^2-20|x|+32/3,  |t|<2
   0,                               |t|>=2
  The number of taps is intentionally kept small to reduce computational
   overhead and limit ringing.

  The taps from these filters are scaled so that their sum is 1, and the result
   is scaled by 128 and rounded to integers to create a filter whose
   intermediate values fit inside 16 bits.
  Coefficients are rounded in such a way as to ensure their sum is still 128,
   which is usually equivalent to normal rounding.*/

/*420jpeg chroma samples are sited like:
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |   BR  |       |   BR  |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |   BR  |       |   BR  |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  420mpeg2 chroma samples are sited like:
  Y-------Y-------Y-------Y-------
  |       |       |       |
  BR      |       BR      |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  BR      |       BR      |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  We use a resampling filter to shift the site locations one quarter pixel (at
   the chroma plane's resolution) to the right.
  The 4:2:2 modes look exactly the same, except there are twice as many chroma
   lines, and they are vertically co-sited with the luma samples in both the
   mpeg2 and jpeg cases (thus requiring no vertical resampling).*/
static void y4m_convert_42xmpeg2_42xjpeg(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  int c_w;
  int c_h;
  int pli;
  int y;
  int x;
  /*Skip past the luma data.*/
  _dst+=_y4m->pic_w*_y4m->pic_h;
  /*Compute the size of each chroma plane.*/
  c_w=(_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h;
  c_h=(_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v;
  for(pli=1;pli<3;pli++){
    for(y=0;y<c_h;y++){
      /*Filter: [4 -17 114 35 -9 1]/128, derived from a 6-tap Lanczos
         window.*/
      for(x=0;x<OC_MINI(c_w,2);x++){
        _dst[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[0]-17*_aux[OC_MAXI(x-1,0)]+
         114*_aux[x]+35*_aux[OC_MINI(x+1,c_w-1)]-9*_aux[OC_MINI(x+2,c_w-1)]+
         _aux[OC_MINI(x+3,c_w-1)]+64)>>7,255);
      }
      for(;x<c_w-3;x++){
        _dst[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[x-2]-17*_aux[x-1]+
         114*_aux[x]+35*_aux[x+1]-9*_aux[x+2]+_aux[x+3]+64)>>7,255);
      }
      for(;x<c_w;x++){
        _dst[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[x-2]-17*_aux[x-1]+
         114*_aux[x]+35*_aux[OC_MINI(x+1,c_w-1)]-9*_aux[OC_MINI(x+2,c_w-1)]+
         _aux[c_w-1]+64)>>7,255);
      }
      _dst+=c_w;
      _aux+=c_w;
    }
  }
}

/*This format is only used for interlaced content, but is included for
   completeness.

  420jpeg chroma samples are sited like:
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |   BR  |       |   BR  |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |   BR  |       |   BR  |
  |       |       |       |
  Y-------Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  420paldv chroma samples are sited like:
  YR------Y-------YR------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YB------Y-------YB------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YR------Y-------YR------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YB------Y-------YB------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  We use a resampling filter to shift the site locations one quarter pixel (at
   the chroma plane's resolution) to the right.
  Then we use another filter to move the C_r location down one quarter pixel,
   and the C_b location up one quarter pixel.*/
static void y4m_convert_42xpaldv_42xjpeg(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  unsigned char *tmp;
  int            c_w;
  int            c_h;
  int            c_sz;
  int            pli;
  int            y;
  int            x;
  /*Skip past the luma data.*/
  _dst+=_y4m->pic_w*_y4m->pic_h;
  /*Compute the size of each chroma plane.*/
  c_w=(_y4m->pic_w+1)/2;
  c_h=(_y4m->pic_h+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h;
  c_sz=c_w*c_h;
  /*First do the horizontal re-sampling.
    This is the same as the mpeg2 case, except that after the horizontal case,
     we need to apply a second vertical filter.*/
  tmp=_aux+2*c_sz;
  for(pli=1;pli<3;pli++){
    for(y=0;y<c_h;y++){
      /*Filter: [4 -17 114 35 -9 1]/128, derived from a 6-tap Lanczos
         window.*/
      for(x=0;x<OC_MINI(c_w,2);x++){
        tmp[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[0]-17*_aux[OC_MAXI(x-1,0)]+
         114*_aux[x]+35*_aux[OC_MINI(x+1,c_w-1)]-9*_aux[OC_MINI(x+2,c_w-1)]+
         _aux[OC_MINI(x+3,c_w-1)]+64)>>7,255);
      }
      for(;x<c_w-3;x++){
        tmp[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[x-2]-17*_aux[x-1]+
         114*_aux[x]+35*_aux[x+1]-9*_aux[x+2]+_aux[x+3]+64)>>7,255);
      }
      for(;x<c_w;x++){
        tmp[x]=(unsigned char)OC_CLAMPI(0,(4*_aux[x-2]-17*_aux[x-1]+
         114*_aux[x]+35*_aux[OC_MINI(x+1,c_w-1)]-9*_aux[OC_MINI(x+2,c_w-1)]+
         _aux[c_w-1]+64)>>7,255);
      }
      tmp+=c_w;
      _aux+=c_w;
    }
    switch(pli){
      case 1:{
        tmp-=c_sz;
        /*Slide C_b up a quarter-pel.
          This is the same filter used above, but in the other order.*/
        for(x=0;x<c_w;x++){
          for(y=0;y<OC_MINI(c_h,3);y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(tmp[0]-
             9*tmp[OC_MAXI(y-2,0)*c_w]+35*tmp[OC_MAXI(y-1,0)*c_w]+
             114*tmp[y*c_w]-17*tmp[OC_MINI(y+1,c_h-1)*c_w]+
             4*tmp[OC_MINI(y+2,c_h-1)*c_w]+64)>>7,255);
          }
          for(;y<c_h-2;y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(tmp[(y-3)*c_w]-
             9*tmp[(y-2)*c_w]+35*tmp[(y-1)*c_w]+114*tmp[y*c_w]-
             17*tmp[(y+1)*c_w]+4*tmp[(y+2)*c_w]+64)>>7,255);
          }
          for(;y<c_h;y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(tmp[(y-3)*c_w]-
             9*tmp[(y-2)*c_w]+35*tmp[(y-1)*c_w]+114*tmp[y*c_w]-
             17*tmp[OC_MINI(y+1,c_h-1)*c_w]+4*tmp[(c_h-1)*c_w]+64)>>7,255);
          }
          _dst++;
          tmp++;
        }
        _dst+=c_sz-c_w;
        tmp-=c_w;
      }break;
      case 2:{
        tmp-=c_sz;
        /*Slide C_r down a quarter-pel.
          This is the same as the horizontal filter.*/
        for(x=0;x<c_w;x++){
          for(y=0;y<OC_MINI(c_h,2);y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(4*tmp[0]-
             17*tmp[OC_MAXI(y-1,0)*c_w]+114*tmp[y*c_w]+
             35*tmp[OC_MINI(y+1,c_h-1)*c_w]-9*tmp[OC_MINI(y+2,c_h-1)*c_w]+
             tmp[OC_MINI(y+3,c_h-1)*c_w]+64)>>7,255);
          }
          for(;y<c_h-3;y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(4*tmp[(y-2)*c_w]-
             17*tmp[(y-1)*c_w]+114*tmp[y*c_w]+35*tmp[(y+1)*c_w]-
             9*tmp[(y+2)*c_w]+tmp[(y+3)*c_w]+64)>>7,255);
          }
          for(;y<c_h;y++){
            _dst[y*c_w]=(unsigned char)OC_CLAMPI(0,(4*tmp[(y-2)*c_w]-
             17*tmp[(y-1)*c_w]+114*tmp[y*c_w]+35*tmp[OC_MINI(y+1,c_h-1)*c_w]-
             9*tmp[OC_MINI(y+2,c_h-1)*c_w]+tmp[(c_h-1)*c_w]+64)>>7,255);
          }
          _dst++;
          tmp++;
        }
      }break;
    }
    /*For actual interlaced material, this would have to be done separately on
       each field, and the shift amounts would be different.
      C_r moves down 1/8, C_b up 3/8 in the top field, and C_r moves down 3/8,
       C_b up 1/8 in the bottom field.
      The corresponding filters would be:
       Down 1/8 (reverse order for up): [3 -11 125 15 -4 0]/128
       Down 3/8 (reverse order for up): [4 -19 98 56 -13 2]/128*/
  }
}

/*422jpeg chroma samples are sited like:
  Y---BR--Y-------Y---BR--Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y---BR--Y-------Y---BR--Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y---BR--Y-------Y---BR--Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  Y---BR--Y-------Y---BR--Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  411 chroma samples are sited like:
  YBR-----Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YBR-----Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YBR-----Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |
  YBR-----Y-------Y-------Y-------
  |       |       |       |
  |       |       |       |
  |       |       |       |

  We use a filter to resample at site locations one eighth pixel (at the source
   chroma plane's horizontal resolution) and five eighths of a pixel to the
   right.*/
static void y4m_convert_411_422jpeg(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  int c_w;
  int dst_c_w;
  int c_h;
  int pli;
  int y;
  int x;
  /*Skip past the luma data.*/
  _dst+=_y4m->pic_w*_y4m->pic_h;
  /*Compute the size of each chroma plane.*/
  c_w=(_y4m->pic_w+_y4m->src_c_dec_h-1)/_y4m->src_c_dec_h;
  dst_c_w=(_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h;
  c_h=(_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v;
  for(pli=1;pli<3;pli++){
    for(y=0;y<c_h;y++){
      /*Filters: [1 110 18 -1]/128 and [-3 50 86 -5]/128, both derived from a
         4-tap Mitchell window.*/
      for(x=0;x<OC_MINI(c_w,1);x++){
        _dst[x<<1]=(unsigned char)OC_CLAMPI(0,(111*_aux[0]+
         18*_aux[OC_MINI(1,c_w-1)]-_aux[OC_MINI(2,c_w-1)]+64)>>7,255);
        _dst[x<<1|1]=(unsigned char)OC_CLAMPI(0,(47*_aux[0]+
         86*_aux[OC_MINI(1,c_w-1)]-5*_aux[OC_MINI(2,c_w-1)]+64)>>7,255);
      }
      for(;x<c_w-2;x++){
        _dst[x<<1]=(unsigned char)OC_CLAMPI(0,(_aux[x-1]+110*_aux[x]+
         18*_aux[x+1]-_aux[x+2]+64)>>7,255);
        _dst[x<<1|1]=(unsigned char)OC_CLAMPI(0,(-3*_aux[x-1]+50*_aux[x]+
         86*_aux[x+1]-5*_aux[x+2]+64)>>7,255);
      }
      for(;x<c_w;x++){
        _dst[x<<1]=(unsigned char)OC_CLAMPI(0,(_aux[x-1]+110*_aux[x]+
         18*_aux[OC_MINI(x+1,c_w-1)]-_aux[c_w-1]+64)>>7,255);
        if((x<<1|1)<dst_c_w){
          _dst[x<<1|1]=(unsigned char)OC_CLAMPI(0,(-3*_aux[x-1]+50*_aux[x]+
           86*_aux[OC_MINI(x+1,c_w-1)]-5*_aux[c_w-1]+64)>>7,255);
        }
      }
      _dst+=dst_c_w;
      _aux+=c_w;
    }
  }
}

/*The image is padded with empty chroma components at 4:2:0.
  This costs about 17 bits a frame to code.*/
static void y4m_convert_mono_420jpeg(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  int c_sz;
  (void)_aux;
  _dst+=_y4m->pic_w*_y4m->pic_h;
  c_sz=((_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h)*
   ((_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v);
  memset(_dst,128,c_sz*2);
}

#if 0
/*Right now just 444 to 420.
  Not too hard to generalize.*/
static void y4m_convert_4xxjpeg_42xjpeg(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  unsigned char *tmp;
  int            c_w;
  int            c_h;
  int            pic_sz;
  int            tmp_sz;
  int            c_sz;
  int            pli;
  int            y;
  int            x;
  /*Compute the size of each chroma plane.*/
  c_w=(_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h;
  c_h=(_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v;
  pic_sz=_y4m->pic_w*_y4m->pic_h;
  tmp_sz=c_w*_y4m->pic_h;
  c_sz=c_w*c_h;
  _dst+=pic_sz;
  for(pli=1;pli<3;pli++){
    tmp=_aux+pic_sz;
    /*In reality, the horizontal and vertical steps could be pipelined, for
       less memory consumption and better cache performance, but we do them
       separately for simplicity.*/
    /*First do horizontal filtering (convert to 4:2:2)*/
    /*Filter: [3 -17 78 78 -17 3]/128, derived from a 6-tap Lanczos window.*/
    for(y=0;y<_y4m->pic_h;y++){
      for(x=0;x<OC_MINI(_y4m->pic_w,2);x+=2){
        tmp[x>>1]=OC_CLAMPI(0,64*_aux[0]+78*_aux[OC_MINI(1,_y4m->pic_w-1)]
         -17*_aux[OC_MINI(2,_y4m->pic_w-1)]
         +3*_aux[OC_MINI(3,_y4m->pic_w-1)]+64>>7,255);
      }
      for(;x<_y4m->pic_w-3;x+=2){
        tmp[x>>1]=OC_CLAMPI(0,3*(_aux[x-2]+_aux[x+3])-17*(_aux[x-1]+_aux[x+2])+
         78*(_aux[x]+_aux[x+1])+64>>7,255);
      }
      for(;x<_y4m->pic_w;x+=2){
        tmp[x>>1]=OC_CLAMPI(0,3*(_aux[x-2]+_aux[_y4m->pic_w-1])-
         17*(_aux[x-1]+_aux[OC_MINI(x+2,_y4m->pic_w-1)])+
         78*(_aux[x]+_aux[OC_MINI(x+1,_y4m->pic_w-1)])+64>>7,255);
      }
      tmp+=c_w;
      _aux+=_y4m->pic_w;
    }
    _aux-=pic_sz;
    tmp-=tmp_sz;
    /*Now do the vertical filtering.*/
    for(x=0;x<c_w;x++){
      for(y=0;y<OC_MINI(_y4m->pic_h,2);y+=2){
        _dst[(y>>1)*c_w]=OC_CLAMPI(0,64*tmp[0]
         +78*tmp[OC_MINI(1,_y4m->pic_h-1)*c_w]
         -17*tmp[OC_MINI(2,_y4m->pic_h-1)*c_w]
         +3*tmp[OC_MINI(3,_y4m->pic_h-1)*c_w]+64>>7,255);
      }
      for(;y<_y4m->pic_h-3;y+=2){
        _dst[(y>>1)*c_w]=OC_CLAMPI(0,3*(tmp[(y-2)*c_w]+tmp[(y+3)*c_w])-
         17*(tmp[(y-1)*c_w]+tmp[(y+2)*c_w])+78*(tmp[y*c_w]+tmp[(y+1)*c_w])+
         64>>7,255);
      }
      for(;y<_y4m->pic_h;y+=2){
        _dst[(y>>1)*c_w]=OC_CLAMPI(0,3*(tmp[(y-2)*c_w]
         +tmp[(_y4m->pic_h-1)*c_w])-17*(tmp[(y-1)*c_w]
         +tmp[OC_MINI(y+2,_y4m->pic_h-1)*c_w])
         +78*(tmp[y*c_w]+tmp[OC_MINI(y+1,_y4m->pic_h-1)*c_w])+64>>7,255);
      }
      tmp++;
      _dst++;
    }
    _dst-=c_w;
  }
}
#endif

/*No conversion function needed.*/
static void y4m_convert_null(y4m_input *_y4m,unsigned char *_dst,
 unsigned char *_aux){
  (void)_y4m;
  (void)_dst;
  (void)_aux;
}

#define Y4M_HEADER_BUFSIZE 256

static int y4m_input_open_impl(y4m_input *_y4m,FILE *_fin){
  char buffer[Y4M_HEADER_BUFSIZE];
  int  ret;
  int  i;
  int  xstride;
  /*Read until newline, or Y4M_HEADER_BUFSIZE cols, whichever happens first.*/
  for(i=0;i<Y4M_HEADER_BUFSIZE-1;i++){
    ret=fread(buffer+i,1,1,_fin);
    if(ret<1)return -1;
    if(buffer[i]=='\n')break;
  }
  buffer[i]='\0';
  if(memcmp(buffer,"YUV4MPEG",8)){
    fprintf(stderr,"Incomplete magic for YUV4MPEG file.\n");
    return -1;
  }
  if(buffer[8]!='2'){
    fprintf(stderr,"Incorrect YUV input file version; YUV4MPEG2 required.\n");
  }
  ret=y4m_parse_tags(_y4m,buffer+5);
  if(ret<0){
    fprintf(stderr,"Error parsing YUV4MPEG2 header.\n");
    return ret;
  }
  if(_y4m->interlace=='?'){
    fprintf(stderr,"Warning: Input video interlacing format unknown; "
     "assuming progressive scan.\n");
  }
  else if(_y4m->interlace!='p'){
    fprintf(stderr,"Input video is interlaced; "
     "Theora only handles progressive scan.\n");
    return -1;
  }
  _y4m->depth=8;
  if(strcmp(_y4m->chroma_type,"420")==0||
   strcmp(_y4m->chroma_type,"420jpeg")==0||
   strcmp(_y4m->chroma_type,"420mpeg2")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=2;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h
     +2*((_y4m->pic_w+1)/2)*((_y4m->pic_h+1)/2);
    /*Natively supported: no conversion required.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=0;
    _y4m->convert=y4m_convert_null;
  }
  else if(strcmp(_y4m->chroma_type,"420p10")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=2;
    _y4m->dst_buf_read_sz=(_y4m->pic_w*_y4m->pic_h
                           +2*((_y4m->pic_w+1)/2)*((_y4m->pic_h+1)/2))*2;
    _y4m->depth=10;
    /*Natively supported: no conversion required.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=0;
    _y4m->convert=y4m_convert_null;
  }
  else if (strcmp(_y4m->chroma_type,"422p10")==0) {
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=2;
    _y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->depth = 10;
    _y4m->dst_buf_read_sz = 2*(_y4m->pic_w*_y4m->pic_h
		    +2*((_y4m->pic_w+1)/2)*_y4m->pic_h);
    _y4m->aux_buf_sz = _y4m->aux_buf_read_sz = 0;
    _y4m->convert = y4m_convert_null;
  }
  else if(strcmp(_y4m->chroma_type,"444p10")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h*3*2;
    _y4m->depth=10;
    /*Natively supported: no conversion required.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=0;
    _y4m->convert=y4m_convert_null;
  }
  else if(strcmp(_y4m->chroma_type,"420paldv")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=2;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h;
    /*Chroma filter required: read into the aux buf first.
      We need to make two filter passes, so we need some extra space in the
       aux buffer.*/
    _y4m->aux_buf_sz=3*((_y4m->pic_w+1)/2)*((_y4m->pic_h+1)/2);
    _y4m->aux_buf_read_sz=2*((_y4m->pic_w+1)/2)*((_y4m->pic_h+1)/2);
    _y4m->convert=y4m_convert_42xpaldv_42xjpeg;
  }
  else if(strcmp(_y4m->chroma_type,"422")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=2;
    _y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h;
    /*Chroma filter required: read into the aux buf first.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=2*((_y4m->pic_w+1)/2)*_y4m->pic_h;
    _y4m->convert=y4m_convert_42xmpeg2_42xjpeg;
  }
  else if(strcmp(_y4m->chroma_type,"411")==0){
    _y4m->src_c_dec_h=4;
    /*We don't want to introduce any additional sub-sampling, so we
       promote 4:1:1 material to 4:2:2, as the closest format Theora can
       handle.*/
    _y4m->dst_c_dec_h=2;
    _y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h;
    /*Chroma filter required: read into the aux buf first.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=2*((_y4m->pic_w+3)/4)*_y4m->pic_h;
    _y4m->convert=y4m_convert_411_422jpeg;
  }
  else if(strcmp(_y4m->chroma_type,"444")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h*3;
    /*Natively supported: no conversion required.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=0;
    _y4m->convert=y4m_convert_null;
  }
  else if(strcmp(_y4m->chroma_type,"444alpha")==0){
    _y4m->src_c_dec_h=_y4m->dst_c_dec_h=_y4m->src_c_dec_v=_y4m->dst_c_dec_v=1;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h*3;
    /*Read the extra alpha plane into the aux buf.
      It will be discarded.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=_y4m->pic_w*_y4m->pic_h;
    _y4m->convert=y4m_convert_null;
  }
  else if(strcmp(_y4m->chroma_type,"mono")==0){
    _y4m->src_c_dec_h=_y4m->src_c_dec_v=0;
    _y4m->dst_c_dec_h=_y4m->dst_c_dec_v=2;
    _y4m->dst_buf_read_sz=_y4m->pic_w*_y4m->pic_h;
    /*No extra space required, but we need to clear the chroma planes.*/
    _y4m->aux_buf_sz=_y4m->aux_buf_read_sz=0;
    _y4m->convert=y4m_convert_mono_420jpeg;
  }
  else{
    fprintf(stderr,"Unknown chroma sampling type: %s\n",_y4m->chroma_type);
    return -1;
  }
  xstride = (_y4m->depth>8)?2:1;
  /*The size of the final frame buffers is always computed from the
     destination chroma decimation type.*/
  _y4m->dst_buf_sz=_y4m->pic_w*_y4m->pic_h
   +2*((_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h)*
   ((_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v);
  _y4m->dst_buf_sz*=xstride;
  /*Scale the picture size up to a multiple of 16.*/
  _y4m->frame_w = (_y4m->pic_w + 15) & ~0xF;
  _y4m->frame_h = (_y4m->pic_h + 15) & ~0xF;
  /*Force the offsets to be even so that chroma samples line up like we
     expect.*/
  _y4m->pic_x=(_y4m->frame_w-_y4m->pic_w)>>1&~1;
  _y4m->pic_y=(_y4m->frame_h-_y4m->pic_h)>>1&~1;
  _y4m->dst_buf=(unsigned char *)malloc(_y4m->dst_buf_sz);
  _y4m->aux_buf=_y4m->aux_buf_sz?(unsigned char *)malloc(_y4m->aux_buf_sz):NULL;
  return 0;
}

static y4m_input *y4m_input_open(FILE *_fin){
  y4m_input *y4m = (y4m_input *) malloc(sizeof(*y4m));
  if(y4m==NULL){
    fprintf(stderr,"Could not allocate y4m reader state.\n");
    return NULL;
  }
  if(y4m_input_open_impl(y4m,_fin)<0){
    fprintf(stderr,"Error opening y4m file.\n");
    free(y4m);
    return NULL;
  }
  return y4m;
}

static void y4m_input_get_info(y4m_input *_y4m,video_input_info *_info){
  _info->frame_w=_y4m->frame_w;
  _info->frame_h=_y4m->frame_h;
  _info->pic_w=_y4m->pic_w;
  _info->pic_h=_y4m->pic_h;
  _info->pic_x=_y4m->pic_x;
  _info->pic_y=_y4m->pic_y;
  _info->fps_n=_y4m->fps_n;
  _info->fps_d=_y4m->fps_d;
  _info->par_n=_y4m->par_n;
  _info->par_d=_y4m->par_d;
  _info->pixel_fmt=_y4m->dst_c_dec_h==2?
   (_y4m->dst_c_dec_v==2?PF_420:PF_422):PF_444;
  _info->depth=_y4m->depth;
}

static int y4m_input_fetch_frame(y4m_input *_y4m,FILE *_fin,
 video_input_ycbcr _ycbcr,char _tag[5]){
  char frame[6];
  int  pic_sz;
  int  frame_c_w;
  int  frame_c_h;
  int  c_w;
  int  c_h;
  int  c_sz;
  int  ret;
  int  xstride;
  xstride=(_y4m->depth>8)?2:1;
  pic_sz=_y4m->pic_w*_y4m->pic_h*xstride;
  frame_c_w=_y4m->frame_w/_y4m->dst_c_dec_h;
  frame_c_h=_y4m->frame_h/_y4m->dst_c_dec_v;
  c_w=(_y4m->pic_w+_y4m->dst_c_dec_h-1)/_y4m->dst_c_dec_h;
  c_h=(_y4m->pic_h+_y4m->dst_c_dec_v-1)/_y4m->dst_c_dec_v;
  c_sz=c_w*c_h*xstride;
  /*Read and skip the frame header.*/
  ret=fread(frame,1,6,_fin);
  if(ret<6)return 0;
  if(memcmp(frame,"FRAME",5)){
    fprintf(stderr,"Loss of framing in YUV input data\n");
    return -1;
  }
  if(frame[5]!='\n'){
    char c;
    int  j;
    for(j=0;j<79&&fread(&c,1,1,_fin)&&c!='\n';j++);
    if(j==79){
      fprintf(stderr,"Error parsing YUV frame header\n");
      return -1;
    }
  }
  /*Read the frame data that needs no conversion.*/
  if(fread(_y4m->dst_buf,1,_y4m->dst_buf_read_sz,_fin)!=_y4m->dst_buf_read_sz){
    fprintf(stderr,"Error reading YUV frame data.\n");
    return -1;
  }
  /*Read the frame data that does need conversion.*/
  if(fread(_y4m->aux_buf,1,_y4m->aux_buf_read_sz,_fin)!=_y4m->aux_buf_read_sz){
    fprintf(stderr,"Error reading YUV frame data.\n");
    return -1;
  }
  /*Now convert the just read frame.*/
  (*_y4m->convert)(_y4m,_y4m->dst_buf,_y4m->aux_buf);
  /*Fill in the frame buffer pointers.*/
  _ycbcr[0].width=_y4m->frame_w;
  _ycbcr[0].height=_y4m->frame_h;
  _ycbcr[0].stride=_y4m->pic_w*xstride;
  _ycbcr[0].data=_y4m->dst_buf-(_y4m->pic_x+_y4m->pic_y*_y4m->pic_w)*xstride;
  _ycbcr[1].width=frame_c_w;
  _ycbcr[1].height=frame_c_h;
  _ycbcr[1].stride=c_w*xstride;
  _ycbcr[1].data=_y4m->dst_buf+pic_sz-((_y4m->pic_x/_y4m->dst_c_dec_h)+
   (_y4m->pic_y/_y4m->dst_c_dec_v)*c_w)*xstride;
  _ycbcr[2].width=frame_c_w;
  _ycbcr[2].height=frame_c_h;
  _ycbcr[2].stride=c_w*xstride;
  _ycbcr[2].data=_ycbcr[1].data+c_sz;
  if(_tag!=NULL)_tag[0]='\0';
  return 1;
}

static void y4m_input_close(y4m_input *_y4m){
  free(_y4m->dst_buf);
  free(_y4m->aux_buf);
}

OC_EXTERN const video_input_vtbl Y4M_INPUT_VTBL={
  (video_input_open_func)y4m_input_open,
  (video_input_get_info_func)y4m_input_get_info,
  (video_input_fetch_frame_func)y4m_input_fetch_frame,
  (video_input_close_func)y4m_input_close
};
