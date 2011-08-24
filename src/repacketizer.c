/* Copyright (c) 2011 Xiph.Org Foundation
   Written by Jean-Marc Valin */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "opus_private.h"

struct OpusRepacketizer {
   unsigned char toc;
   int nb_frames;
   const unsigned char *frames[48];
   short len[48];
   int framesize;
};



int opus_repacketizer_get_size(void)
{
   return sizeof(OpusRepacketizer);
}

OpusRepacketizer *opus_repacketizer_init(OpusRepacketizer *rp)
{
   rp->nb_frames = 0;
   return rp;
}

OpusRepacketizer *opus_repacketizer_create(void)
{
   return opus_repacketizer_init(malloc(opus_repacketizer_get_size()));
}

void opus_repacketizer_destroy(OpusRepacketizer *rp)
{
   free(rp);
}

int opus_repacketizer_cat(OpusRepacketizer *rp, const unsigned char *data, int len)
{
   unsigned char tmp_toc;
   int curr_nb_frames;
   /* Set of check ToC */
   if (rp->nb_frames == 0)
   {
      rp->toc = data[0];
      rp->framesize = opus_packet_get_samples_per_frame(data, 48000);
   } else if (rp->toc&0xFC != data[0]&0xFC)
   {
      /*fprintf(stderr, "toc mismatch: 0x%x vs 0x%x\n", rp->toc, data[0]);*/
      return OPUS_CORRUPTED_DATA;
   }
   curr_nb_frames = opus_packet_get_nb_frames(data, len);

   /* Check the 120 ms maximum packet size */
   if ((curr_nb_frames+rp->nb_frames)*rp->framesize > 5760)
   {
      return OPUS_CORRUPTED_DATA;
   }

   opus_packet_parse(data, len, &tmp_toc, &rp->frames[rp->nb_frames], &rp->len[rp->nb_frames], NULL);

   rp->nb_frames += curr_nb_frames;
   return OPUS_OK;
}

int opus_repacketizer_get_nb_frames(OpusRepacketizer *rp)
{
   return rp->nb_frames;
}

int opus_repacketizer_out_range(OpusRepacketizer *rp, int begin, int end, unsigned char *data, int maxlen)
{
   int i, count, tot_size;
   short *len;
   const unsigned char **frames;

   if (begin<0 || begin>=end || end>rp->nb_frames)
   {
      /*fprintf(stderr, "%d %d %d\n", begin, end, rp->nb_frames);*/
      return OPUS_BAD_ARG;
   }
   count = end-begin;

   len = rp->len+begin;
   frames = rp->frames+begin;
   switch (count)
   {
   case 1:
   {
      /* Code 0 */
      tot_size = len[0]+1;
      if (tot_size > maxlen)
         return OPUS_BUFFER_TOO_SMALL;
      *data++ = rp->toc&0xFC;
   }
   break;
   case 2:
   {
      if (len[1] == len[0])
      {
         /* Code 1 */
         tot_size = 2*len[0]+1;
         if (tot_size > maxlen)
            return OPUS_BUFFER_TOO_SMALL;
         *data++ = (rp->toc&0xFC) | 0x1;
      } else {
         /* Code 2 */
         tot_size = len[0]+len[1]+2+(len[0]>=252);
         if (tot_size > maxlen)
            return OPUS_BUFFER_TOO_SMALL;
         *data++ = (rp->toc&0xFC) | 0x2;
         data += encode_size(len[0], data);
      }
   }
   break;
   default:
   {
      /* Code 3 */
      int vbr;

      vbr = 0;
      for (i=1;i<count;i++)
      {
         if (len[i] != len[0])
         {
            vbr=1;
            break;
         }
      }
      if (vbr)
      {
         tot_size = 2;
         for (i=0;i<count-1;i++)
            tot_size += 1 + (len[i]>=252) + len[i];
         tot_size += len[count-1];

         if (tot_size > maxlen)
            return OPUS_BUFFER_TOO_SMALL;
         *data++ = (rp->toc&0xFC) | 0x3;
         *data++ = count | 0x80;
         for (i=0;i<count-1;i++)
            data += encode_size(len[i], data);
      } else {
         tot_size = count*len[0]+2;
         if (tot_size > maxlen)
            return OPUS_BUFFER_TOO_SMALL;
         *data++ = (rp->toc&0xFC) | 0x3;
         *data++ = count;
      }
   }
   }
   /* Copy the actual data */
   for (i=0;i<count;i++)
   {
      memcpy(data, frames[i], len[i]);
      data += len[i];
   }
   return tot_size;
}

int opus_repacketizer_out(OpusRepacketizer *rp, unsigned char *data, int maxlen)
{
   return opus_repacketizer_out_range(rp, 0, rp->nb_frames, data, maxlen);
}

