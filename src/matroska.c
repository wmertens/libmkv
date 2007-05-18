/*****************************************************************************
 * matroska.c:
 *****************************************************************************
 * Copyright (C) 2005 x264 project
 * $Id: $
 *
 * Authors: Mike Matsnev
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#include <inttypes.h>
#endif

#include "libmkv.h"
#include "matroska.h"
#include "config.h"

#define CLSIZE    1048576
#define CHECK(x)  do { if ((x) < 0) return -1; } while (0)


static mk_Context *mk_createContext(mk_Writer *w, mk_Context *parent, unsigned id) {
  mk_Context  *c;

  if (w->freelist) {
    c = w->freelist;
    w->freelist = w->freelist->next;
  } else {
    c = malloc(sizeof(*c));
    memset(c, 0, sizeof(*c));
  }

  if (c == NULL)
    return NULL;

  c->parent = parent;
  c->owner = w;
  c->id = id;

  if (c->owner->actlist)
    c->owner->actlist->prev = &c->next;
  c->next = c->owner->actlist;
  c->prev = &c->owner->actlist;

  return c;
}

static int    mk_appendContextData(mk_Context *c, const void *data, unsigned size) {
  unsigned  ns = c->d_cur + size;

  if (ns > c->d_max) {
    void      *dp;
    unsigned  dn = c->d_max ? c->d_max << 1 : 16;
    while (ns > dn)
      dn <<= 1;

    dp = realloc(c->data, dn);
    if (dp == NULL)
      return -1;

    c->data = dp;
    c->d_max = dn;
  }

  memcpy((char*)c->data + c->d_cur, data, size);

  c->d_cur = ns;

  return 0;
}

static int    mk_writeID(mk_Context *c, unsigned id) {
  unsigned char   c_id[4] = { id >> 24, id >> 16, id >> 8, id };

  if (c_id[0])
    return mk_appendContextData(c, c_id, 4);
  if (c_id[1])
    return mk_appendContextData(c, c_id+1, 3);
  if (c_id[2])
    return mk_appendContextData(c, c_id+2, 2);
  return mk_appendContextData(c, c_id+3, 1);
}

static int    mk_writeSize(mk_Context *c, unsigned size) {
  unsigned char   c_size[5] = { 0x08, size >> 24, size >> 16, size >> 8, size };

  if (size < 0x7f) {
    c_size[4] |= 0x80;
    return mk_appendContextData(c, c_size+4, 1);
  }
  if (size < 0x3fff) {
    c_size[3] |= 0x40;
    return mk_appendContextData(c, c_size+3, 2);
  }
  if (size < 0x1fffff) {
    c_size[2] |= 0x20;
    return mk_appendContextData(c, c_size+2, 3);
  }
  if (size < 0x0fffffff) {
    c_size[1] |= 0x10;
    return mk_appendContextData(c, c_size+1, 4);
  }
  return mk_appendContextData(c, c_size, 5);
}

static int    mk_flushContextID(mk_Context *c) {
  unsigned char ff = 0xff;

  if (c->id == 0)
    return 0;

  CHECK(mk_writeID(c->parent, c->id));
  CHECK(mk_appendContextData(c->parent, &ff, 1));

  c->id = 0;

  return 0;
}

static int    mk_flushContextData(mk_Context *c) {
  if (c->d_cur == 0)
    return 0;

  if (c->parent)
    CHECK(mk_appendContextData(c->parent, c->data, c->d_cur));
  else
    if (fwrite(c->data, c->d_cur, 1, c->owner->fp) != 1)
      return -1;

  c->d_cur = 0;

  return 0;
}

static int    mk_closeContext(mk_Context *c, unsigned *off) {
  if (c->id) {
    CHECK(mk_writeID(c->parent, c->id));
    CHECK(mk_writeSize(c->parent, c->d_cur));
  }

  if (c->parent && off != NULL)
    *off += c->parent->d_cur;

  CHECK(mk_flushContextData(c));

  if (c->next)
    c->next->prev = c->prev;
  *(c->prev) = c->next;
  c->next = c->owner->freelist;
  c->owner->freelist = c;

  return 0;
}

static void   mk_destroyContexts(mk_Writer *w) {
  mk_Context  *cur, *next;

  for (cur = w->freelist; cur; cur = next) {
    next = cur->next;
    free(cur->data);
    free(cur);
  }

  for (cur = w->actlist; cur; cur = next) {
    next = cur->next;
    free(cur->data);
    free(cur);
  }

  w->freelist = w->actlist = w->root = NULL;
}

static int    mk_writeStr(mk_Context *c, unsigned id, const char *str) {
  size_t  len = strlen(str);

  CHECK(mk_writeID(c, id));
  CHECK(mk_writeSize(c, len));
  CHECK(mk_appendContextData(c, str, len));
  return 0;
}

static int    mk_writeBin(mk_Context *c, unsigned id, const void *data, unsigned size) {
  CHECK(mk_writeID(c, id));
  CHECK(mk_writeSize(c, size));
  CHECK(mk_appendContextData(c, data, size));
  return 0;
}

static int    mk_writeUInt(mk_Context *c, unsigned id, uint64_t ui) {
  unsigned char   c_ui[8] = { ui >> 56, ui >> 48, ui >> 40, ui >> 32, ui >> 24, ui >> 16, ui >> 8, ui };
  unsigned    i = 0;

  CHECK(mk_writeID(c, id));
  while (i < 7 && c_ui[i] == 0)
    ++i;
  CHECK(mk_writeSize(c, 8 - i));
  CHECK(mk_appendContextData(c, c_ui+i, 8 - i));
  return 0;
}

static int        mk_writeSInt(mk_Context *c, unsigned id, int64_t si) {
  unsigned char   c_si[8] = { si >> 56, si >> 48, si >> 40, si >> 32, si >> 24, si >> 16, si >> 8, si };
  unsigned    i = 0;

  CHECK(mk_writeID(c, id));
  if (si < 0)
    while (i < 7 && c_si[i] == 0xff && c_si[i+1] & 0x80)
      ++i;
  else
    while (i < 7 && c_si[i] == 0 && !(c_si[i+1] & 0x80))
      ++i;
  CHECK(mk_writeSize(c, 8 - i));
  CHECK(mk_appendContextData(c, c_si+i, 8 - i));
  return 0;
}

static int    mk_writeFloatRaw(mk_Context *c, float f) {
  union {
    float f;
    unsigned u;
  } u;
  unsigned char c_f[4];

  u.f = f;
  c_f[0] = u.u >> 24;
  c_f[1] = u.u >> 16;
  c_f[2] = u.u >> 8;
  c_f[3] = u.u;

  return mk_appendContextData(c, c_f, 4);
}

static int    mk_writeFloat(mk_Context *c, unsigned id, float f) {
  CHECK(mk_writeID(c, id));
  CHECK(mk_writeSize(c, 4));
  CHECK(mk_writeFloatRaw(c, f));
  return 0;
}

static unsigned   mk_ebmlSizeSize(unsigned s) {
  if (s < 0x7f)
    return 1;
  if (s < 0x3fff)
    return 2;
  if (s < 0x1fffff)
    return 3;
  if (s < 0x0fffffff)
    return 4;
  return 5;
}

static unsigned   mk_ebmlSIntSize(int64_t si) {
  unsigned char   c_si[8] = { si >> 56, si >> 48, si >> 40, si >> 32, si >> 24, si >> 16, si >> 8, si };
  unsigned    i = 0;

  if (si < 0)
    while (i < 7 && c_si[i] == 0xff && c_si[i+1] & 0x80)
      ++i;
  else
    while (i < 7 && c_si[i] == 0 && !(c_si[i+1] & 0x80))
      ++i;

  return 8 - i;
}

mk_Writer *mk_createWriter(const char *filename,
                           int64_t timescale)
{
  mk_Writer *w = malloc(sizeof(*w));
  if (w == NULL)
    return NULL;

  memset(w, 0, sizeof(*w));
  
  w->seek_data = malloc(sizeof(mk_Seek));
  if (w->seek_data == NULL)
  {
    free(w);
    return NULL;
  }

  memset(w->seek_data, 0, sizeof(mk_Seek));
  
  w->root = mk_createContext(w, NULL, 0);
  if (w->root == NULL) {
    free(w->seek_data);
    free(w);
    return NULL;
  }

  w->fp = fopen(filename, "wb");
  if (w->fp == NULL) {
    mk_destroyContexts(w);
    free(w->seek_data);
    free(w);
    return NULL;
  }

  w->timescale = timescale;
//  w->def_duration = default_frame_duration;

  return w;
}

mk_Track *mk_createTrack(mk_Writer *w, mk_TrackConfig *tc)
{
    mk_Track *track = calloc(1, sizeof(*track));
    if (track == NULL)
        return NULL;
    track->config = calloc(1, sizeof(mk_TrackConfig));
    if (track->config == NULL)
        return NULL;
    memcpy(track->config, tc, sizeof(mk_TrackConfig));
    if(tc->video)
    {
        track->config->video = calloc(1, sizeof(mk_VideoConfig));
        if (track->config->video == NULL)
            return NULL;
        memcpy(track->config->video, tc->video, sizeof(mk_VideoConfig));
    }
    else if (tc->audio)
    {
        track->config->audio = calloc(1, sizeof(mk_AudioConfig));
        if (track->config->audio == NULL)
            return NULL;
        memcpy(track->config->audio, tc->audio, sizeof(mk_AudioConfig));
    }
    if (tc->codecPrivate && (tc->codecPrivateSize > 0))
    {
        track->config->codecPrivate = calloc(1, tc->codecPrivateSize);
        if (track->config->codecPrivate == NULL)
            return NULL;
        memcpy(track->config->codecPrivate, tc->codecPrivate, tc->codecPrivateSize);
        track->config->codecPrivateSize = tc->codecPrivateSize;
    }
    
    w->tracks = realloc( w->tracks, (w->num_tracks + 1) * sizeof(mk_Track *));
    w->tracks[w->num_tracks] = track;
    track->track_id = w->num_tracks + 1;
    w->num_tracks++;
    
    return track;
}

int   mk_writeHeader(mk_Writer *w, const char *writingApp)
{
  mk_Context  *c, *s;
  int   i;

  if (w->wrote_header)
    return -1;

  if ((c = mk_createContext(w, w->root, 0x1a45dfa3)) == NULL) // EBML
    return -1;
  CHECK(mk_writeUInt(c, 0x4286, 1)); // EBMLVersion
  CHECK(mk_writeUInt(c, 0x42f7, 1)); // EBMLReadVersion
  CHECK(mk_writeUInt(c, 0x42f2, 4)); // EBMLMaxIDLength
  CHECK(mk_writeUInt(c, 0x42f3, 8)); // EBMLMaxSizeLength
  CHECK(mk_writeStr(c, 0x4282, "matroska")); // DocType
  CHECK(mk_writeUInt(c, 0x4287, 1)); // DocTypeVersion
  CHECK(mk_writeUInt(c, 0x4285, 1)); // DocTypeReadversion
  CHECK(mk_closeContext(c, 0));

  if ((c = mk_createContext(w, w->root, 0x18538067)) == NULL) // Segment
    return -1;
  CHECK(mk_flushContextID(c));
//  CHECK(mk_writeSize(c, 0xfffff));                         // Dummy value until we get the actual size. 1TB should be enough.
  CHECK(mk_closeContext(c, 0));
  /* Shouldn't the Segment encapsulate all the rest of the tags, why is it closed here? */

  if ((c = mk_createContext(w, w->root, 0x114d9b74)) == NULL) // SeekHead
    return -1;
  if ((s = mk_createContext(w, c, 0x4dbb)) == NULL) // Seek
    return -1;
  CHECK(mk_writeUInt(s, 0x53ab, 0x114d9b74)); // SeekID
  CHECK(mk_writeUInt(s, 0x53ac, 0xffffffff)); // SeekPosition
  w->seekhead_ptr = s->d_cur - 7;
                                             /* We write a dummy number here so
                                              * there is enough space for our
                                              * actual position. We'll fill
                                              * that in later (mk_closeWriter) */
  CHECK(mk_closeContext(s, &w->seekhead_ptr));
  CHECK(mk_closeContext(c, &w->seekhead_ptr));

  if ((c = mk_createContext(w, w->root, 0x1549a966)) == NULL) // SegmentInfo
    return -1;
  w->seek_data->segmentinfo = w->root->d_cur;
  CHECK(mk_writeStr(c, 0x4d80, PACKAGE_STRING)); // MuxingApp
  CHECK(mk_writeStr(c, 0x5741, writingApp)); // WritingApp
  CHECK(mk_writeUInt(c, 0x2ad7b1, w->timescale)); // TimecodeScale
  CHECK(mk_writeFloat(c, 0x4489, 0)); // Duration
  w->duration_ptr = c->d_cur - 4;
  CHECK(mk_closeContext(c, &w->duration_ptr));

  if ((c = mk_createContext(w, w->root, 0x1654ae6b)) == NULL) // tracks
    return -1;
  w->seek_data->tracks = w->root->d_cur;

  for (i = 0; i < w->num_tracks; i++)
  {
    if (mk_writeTrack(w, c, w->tracks[i]))
      return -1;
  }

  CHECK(mk_closeContext(c, 0));
  CHECK(mk_flushContextData(w->root));

  w->wrote_header = 1;
  w->def_duration = w->tracks[0]->config->defaultDuration;

  return 0;
}

int   mk_writeTrack(mk_Writer *w, mk_Context *tracks, mk_Track *t)
{
    mk_Context  *ti, *v;
    
    if ((ti = mk_createContext(w, tracks, 0xae)) == NULL) // TrackEntry
        return -1;
    CHECK(mk_writeUInt(ti, 0xd7, t->track_id)); // TrackNumber
    if (t->config->trackUID)
        CHECK(mk_writeUInt(ti, 0x73c5, t->config->trackUID)); // TrackUID
    else
        CHECK(mk_writeUInt(ti, 0x73c5, t->track_id));
    CHECK(mk_writeUInt(ti, 0x83, t->config->trackType)); // TrackType
    CHECK(mk_writeUInt(ti, 0x9c, t->config->flagLacing)); // FlagLacing
    CHECK(mk_writeStr(ti, 0x86, t->config->codecID)); // CodecID
    if (t->config->codecPrivateSize)
        CHECK(mk_writeBin(ti, 0x63a2, t->config->codecPrivate, t->config->codecPrivateSize)); // CodecPrivate
//    if (w->def_duration)
//        CHECK(mk_writeUInt(ti, 0x23e383, w->def_duration)); // DefaultDuration
    if (t->config->defaultDuration)
        CHECK(mk_writeUInt(ti, 0x23e383, t->config->defaultDuration));
    if (t->config->language)
        CHECK(mk_writeStr(ti, 0x22b59c, t->config->language));
    switch (t->config->trackType)
    {
        case MK_TRACK_VIDEO:    // Video
            if ((v = mk_createContext(w, ti, 0xe0)) == NULL)
                return -1;
            CHECK(mk_writeUInt(v, 0xb0, t->config->video->width));
            CHECK(mk_writeUInt(v, 0xba, t->config->video->height));
            CHECK(mk_writeUInt(v, 0x54b0, t->config->video->d_width));
            CHECK(mk_writeUInt(v, 0x54ba, t->config->video->d_height));
            break;
        case MK_TRACK_AUDIO:    // Audio
            if ((v = mk_createContext(w, ti, 0xe1)) == NULL)
                return -1;
            CHECK(mk_writeFloat(v, 0xb5, t->config->audio->samplingFreq));
            CHECK(mk_writeUInt(v, 0x9f, t->config->audio->channels));
            if (t->config->audio->bitDepth)
                CHECK(mk_writeUInt(v, 0x6264, t->config->audio->bitDepth));
            break;
        default:                // Other
            return -1;
    }
    CHECK(mk_closeContext(v, 0));

    CHECK(mk_closeContext(ti, 0));

    return 0;
}

static int mk_closeCluster(mk_Writer *w) {
  if (w->cluster == NULL)
    return 0;
  CHECK(mk_closeContext(w->cluster, 0));
  w->cluster = NULL;
  CHECK(mk_flushContextData(w->root));
  return 0;
}

int   mk_flushFrame(mk_Writer *w, mk_Track *track) {
  int64_t   delta, ref = 0;
  unsigned  fsize, bgsize;
  unsigned char c_delta_flags[3];

  if (!track->in_frame)
    return 0;

  delta = track->frame_tc/w->timescale - w->cluster_tc_scaled;
  if (delta > 32767ll || delta < -32768ll)
    CHECK(mk_closeCluster(w));

  if (w->cluster == NULL) {
    w->cluster_tc_scaled = track->frame_tc / w->timescale;
    w->cluster = mk_createContext(w, w->root, 0x1f43b675); // Cluster
    if (w->cluster == NULL)
      return -1;

    CHECK(mk_writeUInt(w->cluster, 0xe7, w->cluster_tc_scaled)); // Cluster Timecode

    delta = 0;
  }

  fsize = track->frame ? track->frame->d_cur : 0;
  bgsize = fsize + 4 + mk_ebmlSizeSize(fsize + 4) + 1;
  if (!track->keyframe) {
    ref = track->prev_frame_tc_scaled - w->cluster_tc_scaled - delta;
    bgsize += 1 + 1 + mk_ebmlSIntSize(ref);
  }

  CHECK(mk_writeID(w->cluster, 0xa0)); // BlockGroup
  CHECK(mk_writeSize(w->cluster, bgsize));
  CHECK(mk_writeID(w->cluster, 0xa1)); // Block
  CHECK(mk_writeSize(w->cluster, fsize + 4));
  CHECK(mk_writeSize(w->cluster, track->track_id)); // track number

  c_delta_flags[0] = delta >> 8;
  c_delta_flags[1] = delta;
  c_delta_flags[2] = 0;
  CHECK(mk_appendContextData(w->cluster, c_delta_flags, 3));
  if (track->frame) {
    CHECK(mk_appendContextData(w->cluster, track->frame->data, track->frame->d_cur));
    track->frame->d_cur = 0;
  }
  if (!track->keyframe)
    CHECK(mk_writeSInt(w->cluster, 0xfb, ref)); // ReferenceBlock

  track->in_frame = 0;
  track->prev_frame_tc_scaled = w->cluster_tc_scaled + delta;

  if (w->cluster->d_cur > CLSIZE)
    CHECK(mk_closeCluster(w));

  return 0;
}

int   mk_startFrame(mk_Writer *w, mk_Track *track) {
  if (mk_flushFrame(w, track) < 0)
    return -1;

  track->in_frame = 1;
  track->keyframe = 0;

  return 0;
}

int   mk_setFrameFlags(mk_Writer *w, mk_Track *track, int64_t timestamp, int keyframe) {
  if (!track->in_frame)
    return -1;

  track->frame_tc = timestamp;
  track->keyframe = keyframe != 0;

  if (track->max_frame_tc < timestamp)
    track->max_frame_tc = timestamp;

  return 0;
}

int   mk_addFrameData(mk_Writer *w, mk_Track *track, const void *data, unsigned size) {
  if (!track->in_frame)
    return -1;

  if (track->frame == NULL)
    if ((track->frame = mk_createContext(w, NULL, 0)) == NULL)
      return -1;

  return mk_appendContextData(track->frame, data, size);
}

int   mk_close(mk_Writer *w) {
  int   i, ret = 0;
  int   def_duration = 0;
  mk_Context *c, *s;
  mk_Track *tk;
  
  for (i = 0; i < w->num_tracks; i++)
  {
    tk = w->tracks[i];
    
    if (mk_flushFrame(w, tk) < 0)
      ret = -1;
    if (tk->config->video)
      free(tk->config->video);
    if (tk->config->audio)
      free(tk->config->audio);
    if (tk->config->codecPrivate != NULL && tk->config->codecPrivateSize > 0)
      free(tk->config->codecPrivate);

    free(tk);
  }
  if (mk_closeCluster(w) < 0)
    ret = -1;
  if (w->wrote_header) {

    if ((c = mk_createContext(w, w->root, 0x114d9b74)) != NULL) // SeekHead
    {
      w->seek_data->seekhead = ftell(w->fp);
      if (w->seek_data->segmentinfo)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1549a966) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->segmentinfo) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
            ret = -1;
        }
      }
      if (w->seek_data->tracks)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1654ae6b) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->tracks) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
            ret = -1;
        }
      }
      if (w->seek_data->cues)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1c53bb6b) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->cues) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
            ret = -1;
        }
      }
      if (w->seek_data->attachments)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1941a469) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->attachments) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
            ret = -1;
        }
      }
      if (w->seek_data->chapters)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1043a770) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->chapters) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
          ret = -1;
        }
      }
      if (w->seek_data->tags)
      {
        if ((s = mk_createContext(w, c, 0x4dbb)) != NULL) // Seek
        {
          if (mk_writeUInt(s, 0x53ab, 0x1254c367) < 0) // SeekID
            ret = -1;
          if (mk_writeUInt(s, 0x53ac, w->seek_data->tags) < 0) // SeekPosition
            ret = -1;
          if (mk_closeContext(s, 0) < 0)
            ret = -1;
        }
      }
      if (mk_closeContext(c, 0) < 0)
        ret = -1;
    }
    if (mk_flushContextData(w->root) < 0)
      ret = -1;

    fseek(w->fp, w->seekhead_ptr, SEEK_SET);
    if (mk_writeUInt(w->root, 0x53ac, w->seek_data->seekhead) < 0 ||
        mk_flushContextData(w->root) < 0)
      ret = -1;

    fseek(w->fp, w->duration_ptr, SEEK_SET);
    if (mk_writeFloatRaw(w->root, (float)((double)(w->tracks[0]->max_frame_tc+w->def_duration) / w->timescale)) < 0 ||
        mk_flushContextData(w->root) < 0)
      ret = -1;
  }

  mk_destroyContexts(w);
  fclose(w->fp);
  free(w->seek_data);
  free(w->tracks);
  free(w);
  
  return ret;
}