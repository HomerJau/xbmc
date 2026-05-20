/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicCodecInfoFFmpeg.h"

#include "cores/FFmpeg.h"
#include "filesystem/File.h"

extern "C"
{
#include <libavutil/pixdesc.h> // av_color_transfer_name
}

using namespace XFILE;

namespace
{
int vfs_file_read(void* h, uint8_t* buf, int size)
{
  CFile* pFile = static_cast<CFile*>(h);
  return pFile->Read(buf, size);
}

int64_t vfs_file_seek(void* h, int64_t pos, int whence)
{
  CFile* pFile = static_cast<CFile*>(h);
  if (whence == AVSEEK_SIZE)
    return pFile->GetLength();
  else
    return pFile->Seek(pos, whence & ~AVSEEK_FORCE);
}

// Matroska files store per-stream bitrate in the 'BPS' metadata tag rather
// than the codec header. codecpar->bit_rate is often 0 for lossless audio in
// Matroska; fall back to BPS. Mirrors xbmc/cores/VideoPlayer/DVDDemuxers/
// DVDDemuxFFmpeg.cpp:1590-1598 — music-side copy per the music-only-
// ownership design. Returns raw bps (callers /1000 for kbps).
int ResolveBitRate(const AVStream* st)
{
  if (st->codecpar->bit_rate > 0)
    return static_cast<int>(st->codecpar->bit_rate);
  if (const AVDictionaryEntry* bps = av_dict_get(st->metadata, "BPS", nullptr, 0);
      bps && bps->value)
    return static_cast<int>(strtol(bps->value, nullptr, 10));
  return 0;
}

// Map FFmpeg color/transfer signalling on a video stream to a Kodi-style HDR
// type tag. Music-side equivalent of xbmc/cores/VideoPlayer/DVDFileInfo.cpp's
// logic — deliberately not shared, to keep music ownership of this codepath
// independent of the video subsystem.
std::string ResolveHdrType(const AVStream* st)
{
  // Dolby Vision takes precedence — signalled via stream side data.
  for (int i = 0; i < st->codecpar->nb_coded_side_data; ++i)
  {
    if (st->codecpar->coded_side_data[i].type == AV_PKT_DATA_DOVI_CONF)
      return "dolbyvision";
  }
  if (st->codecpar->color_trc == AVCOL_TRC_SMPTE2084)
    return "hdr10";
  if (st->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67)
    return "hlg";
  return "";
}

// Best-effort colorimetry detail (FFmpeg's transfer-characteristic name).
// Empty when transfer is unsignalled. Future work may expand with master-
// display luminance etc.
std::string ResolveHdrDetail(const AVStream* st)
{
  if (st->codecpar->color_trc == AVCOL_TRC_UNSPECIFIED)
    return "";
  const char* trcName = av_color_transfer_name(st->codecpar->color_trc);
  return trcName ? trcName : "";
}

// Map an ffmpeg AVStream*'s codec_id + profile to Kodi's display codec name
// (eg "truehd_atmos", "dtshd_ma_x", "eac3_ddp_atmos"). Mirrors the original
// single-stream logic so multi-stream extraction labels each stream identically.
std::string ResolveCodecName(const AVStream* st)
{
  std::string codec_name = avcodec_get_name(st->codecpar->codec_id);
  const int par_profile = st->codecpar->profile;
  if (st->codecpar->codec_id == AV_CODEC_ID_DTS)
  {
    switch (par_profile)
    {
      case AV_PROFILE_DTS_HD_MA_X:        codec_name = "dtshd_ma_x"; break;
      case AV_PROFILE_DTS_HD_MA_X_IMAX:   codec_name = "dtshd_ma_x_imax"; break;
      case AV_PROFILE_DTS_ES:             codec_name = "dts_es"; break;
      case AV_PROFILE_DTS_96_24:          codec_name = "dts_96_24"; break;
      case AV_PROFILE_DTS_HD_HRA:         codec_name = "dtshd_hra"; break;
      case AV_PROFILE_DTS_EXPRESS:        codec_name = "dts_express"; break;
      case AV_PROFILE_DTS_HD_MA:          codec_name = "dtshd_ma"; break;
      default:                            codec_name = "dts"; break;
    }
  }
  if (st->codecpar->codec_id == AV_CODEC_ID_EAC3 && par_profile == AV_PROFILE_EAC3_DDP_ATMOS)
    codec_name = "eac3_ddp_atmos";
  if (st->codecpar->codec_id == AV_CODEC_ID_TRUEHD && par_profile == AV_PROFILE_TRUEHD_ATMOS)
    codec_name = "truehd_atmos";
  return codec_name;
}

void FillCodecInfo(const AVStream* st, const AVFormatContext* fctx, musicCodecInfo& codec_info)
{
  codec_info.codecName = ResolveCodecName(st);
  codec_info.bitRate = ResolveBitRate(st) / 1000;
  codec_info.channels = st->codecpar->ch_layout.nb_channels;
  codec_info.bitsPerSample = (st->codecpar->bits_per_coded_sample != 0)
                                 ? st->codecpar->bits_per_coded_sample
                                 : st->codecpar->bits_per_raw_sample;
  codec_info.sampleRate = st->codecpar->sample_rate;
  // st->duration is in st->time_base units; rescale to whole seconds.
  // Fall back to the container duration when the stream value is unset.
  if (st->duration != AV_NOPTS_VALUE)
    codec_info.duration =
        static_cast<int>(av_rescale_q(st->duration, st->time_base, AVRational{1, 1}));
  else if (fctx->duration != AV_NOPTS_VALUE)
    codec_info.duration = static_cast<int>(fctx->duration / AV_TIME_BASE);
  else
    codec_info.duration = 0;
}

// Shared implementation. \a streams may be nullptr (single-stream callers);
// \a videoStream / \a hasVideoStream may be nullptr (callers that don't
// care about Concert-MKV detection).
bool GetCodecInfoInternal(const std::string& strFileName,
                          musicCodecInfo& codec_info,
                          std::vector<MusicAudioStreamInfo>* streams,
                          int* preferredIndex,
                          MusicVideoStreamInfo* videoStream,
                          bool* hasVideoStream)
{
  bool haveInfo = false;
  CFile file;
  if (!file.Open(strFileName))
    return haveInfo;

  int bufferSize = 4096;
  int blockSize = file.GetChunkSize();
  if (blockSize > 1)
    bufferSize = blockSize;
  uint8_t* buffer = static_cast<uint8_t*>(av_malloc(bufferSize));
  if (!buffer)
    return haveInfo;

  AVIOContext* ioctx =
      avio_alloc_context(buffer, bufferSize, 0, &file, vfs_file_read, nullptr, vfs_file_seek);
  if (!ioctx)
  {
    av_free(buffer);
    return haveInfo;
  }

  AVFormatContext* fctx = avformat_alloc_context();
  if (!fctx)
  {
    av_free(ioctx->buffer);
    av_free(ioctx);
    return haveInfo;
  }
  fctx->pb = ioctx;

  if (file.IoControl(IOControl::SEEK_POSSIBLE, nullptr) != 1)
    ioctx->seekable = 0;

  const AVInputFormat* iformat = nullptr;
  av_probe_input_buffer(ioctx, &iformat, strFileName.c_str(), nullptr, 0, 0);

  if (avformat_open_input(&fctx, strFileName.c_str(), iformat, nullptr) == 0)
  {
    fctx->flags |= AVFMT_FLAG_NOPARSE;
    if (avformat_find_stream_info(fctx, nullptr) >= 0)
    {
      // First pass: find the default audio stream (or fall back to the first).
      // This drives the single-stream codec_info output for the existing callers.
      int defaultIndex = -1;
      int firstIndex = -1;
      for (unsigned int i = 0; i < fctx->nb_streams; ++i)
      {
        if (fctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
          continue;
        if (firstIndex < 0)
          firstIndex = static_cast<int>(i);
        if (defaultIndex < 0 && (fctx->streams[i]->disposition & AV_DISPOSITION_DEFAULT))
          defaultIndex = static_cast<int>(i);
      }
      const int singleIndex = (defaultIndex >= 0) ? defaultIndex : firstIndex;
      if (singleIndex != -1)
      {
        AVStream* st = fctx->streams[singleIndex];
        const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
        if (decoder)
        {
          FillCodecInfo(st, fctx, codec_info);
          haveInfo = true;
        }
      }

      // Second pass: if the caller wants per-stream info, walk every audio stream.
      // Demuxer is already open; this is zero additional I/O.
      if (streams)
      {
        streams->clear();
        for (unsigned int i = 0; i < fctx->nb_streams; ++i)
        {
          AVStream* st = fctx->streams[i];
          if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
          MusicAudioStreamInfo info;
          info.iStreamIndex = static_cast<int>(i);
          info.strCodec = ResolveCodecName(st);
          info.iChannels = st->codecpar->ch_layout.nb_channels;
          info.iSampleRate = st->codecpar->sample_rate;
          info.iBitRate = ResolveBitRate(st) / 1000;
          info.iBitsPerSample = (st->codecpar->bits_per_coded_sample != 0)
                                    ? st->codecpar->bits_per_coded_sample
                                    : st->codecpar->bits_per_raw_sample;
          const AVDictionaryEntry* langEntry = av_dict_get(st->metadata, "language", nullptr, 0);
          if (langEntry && langEntry->value)
            info.strLanguage = langEntry->value;
          info.iFlags = static_cast<uint32_t>(st->disposition);
          streams->push_back(info);
        }
        if (preferredIndex)
          *preferredIndex = singleIndex; // same heuristic as the single-stream pick
      }

      // Third pass (optional): first non-cover-art video stream. Concert-MKV
      // detection lives here — music files normally have zero video streams;
      // a music-library Matroska that carries one is a concert/live MKV.
      // AV_DISPOSITION_ATTACHED_PIC marks cover-art "video" streams that
      // FLAC / MP3 embed for artwork — skip those, they aren't real video.
      if (videoStream)
        *videoStream = MusicVideoStreamInfo{};
      if (hasVideoStream)
        *hasVideoStream = false;
      if (videoStream)
      {
        for (unsigned int i = 0; i < fctx->nb_streams; ++i)
        {
          AVStream* st = fctx->streams[i];
          if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
          if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            continue;
          videoStream->iVideoWidth = st->codecpar->width;
          videoStream->iVideoHeight = st->codecpar->height;
          videoStream->strVideoCodec = avcodec_get_name(st->codecpar->codec_id);
          // Display aspect: SAR * width / height, fall back to width/height
          // when SAR is unset or invalid.
          if (st->codecpar->sample_aspect_ratio.num > 0 &&
              st->codecpar->sample_aspect_ratio.den > 0 && st->codecpar->height > 0)
          {
            videoStream->fVideoAspect =
                static_cast<float>(st->codecpar->sample_aspect_ratio.num *
                                   st->codecpar->width) /
                static_cast<float>(st->codecpar->sample_aspect_ratio.den *
                                   st->codecpar->height);
          }
          else if (st->codecpar->height > 0)
          {
            videoStream->fVideoAspect = static_cast<float>(st->codecpar->width) /
                                        static_cast<float>(st->codecpar->height);
          }
          if (st->duration != AV_NOPTS_VALUE)
            videoStream->iVideoDuration = static_cast<int>(
                av_rescale_q(st->duration, st->time_base, AVRational{1, 1}));
          if (const AVDictionaryEntry* stereoEntry =
                  av_dict_get(st->metadata, "stereo_mode", nullptr, 0))
          {
            if (stereoEntry->value)
              videoStream->strStereoMode = stereoEntry->value;
          }
          if (const AVDictionaryEntry* langEntry =
                  av_dict_get(st->metadata, "language", nullptr, 0))
          {
            if (langEntry->value)
              videoStream->strVideoLanguage = langEntry->value;
          }
          videoStream->strHdrType = ResolveHdrType(st);
          videoStream->strHdrDetail = ResolveHdrDetail(st);
          if (hasVideoStream)
            *hasVideoStream = true;
          break; // first real video stream wins
        }
      }
    }
    avformat_close_input(&fctx);
  }
  av_free(ioctx->buffer);
  av_free(ioctx);
  return haveInfo;
}
} // namespace

bool CMusicCodecInfoFFmpeg::GetMusicCodecInfo(const std::string& strFileName,
                                              musicCodecInfo& codec_info)
{
  return GetCodecInfoInternal(strFileName, codec_info, nullptr, nullptr, nullptr, nullptr);
}

bool CMusicCodecInfoFFmpeg::GetMusicCodecInfo(const std::string& strFileName,
                                              musicCodecInfo& codec_info,
                                              std::vector<MusicAudioStreamInfo>& streams,
                                              int& preferredIndex)
{
  preferredIndex = -1;
  return GetCodecInfoInternal(strFileName, codec_info, &streams, &preferredIndex, nullptr,
                              nullptr);
}

bool CMusicCodecInfoFFmpeg::GetMusicCodecInfo(const std::string& strFileName,
                                              musicCodecInfo& codec_info,
                                              std::vector<MusicAudioStreamInfo>& streams,
                                              int& preferredIndex,
                                              MusicVideoStreamInfo& videoStream,
                                              bool& hasVideoStream)
{
  preferredIndex = -1;
  hasVideoStream = false;
  return GetCodecInfoInternal(strFileName, codec_info, &streams, &preferredIndex, &videoStream,
                              &hasVideoStream);
}
