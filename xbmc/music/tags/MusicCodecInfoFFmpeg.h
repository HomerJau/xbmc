/*
 *  Copyright (C) 2005-2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "music/Song.h" // for MusicAudioStreamInfo

#include <string>
#include <vector>

struct musicCodecInfo
{
public:
  int bitsPerSample = 0;
  int sampleRate = 0;
  int bitRate = 0;
  int channels = 0;
  int duration = 0;
  std::string codecName;
};

class CMusicCodecInfoFFmpeg
{
public:
  /*!
   \brief Fill codec_info from the file's default (or first) audio stream.
   For Matroska files this is the existing single-stream extraction used by
   MusicInfoTagLoaderMatroska / AudioBookFileDirectory / etc.
   */
  static bool GetMusicCodecInfo(const std::string& strFileName, musicCodecInfo& codec_info);

  /*!
   \brief Same as above plus, on the same demuxer open, collect per-audio-stream
   metadata into \a streams (preferred-stream-index returned via \a preferredIndex,
   set to the AV_DISPOSITION_DEFAULT stream when present, else stream 0).
   \a streams is left empty when the file contains only one audio stream — callers
   may use that as a cheap "single-stream" check.
   */
  static bool GetMusicCodecInfo(const std::string& strFileName,
                                musicCodecInfo& codec_info,
                                std::vector<MusicAudioStreamInfo>& streams,
                                int& preferredIndex);

  /*!
   \brief Same as the audio-streams overload plus video-stream extraction.
   Populates \a videoStream with the first non-cover-art video stream's
   metadata when the container carries one, and sets \a hasVideoStream true.
   Music files with no video stream leave the struct default-constructed and
   \a hasVideoStream false. Used by the scanner to populate Concert-MKV
   metadata (iStreamType = 0 rows in streamdetails) on the same demuxer open
   as the audio-streams pass — zero additional I/O.
   */
  static bool GetMusicCodecInfo(const std::string& strFileName,
                                musicCodecInfo& codec_info,
                                std::vector<MusicAudioStreamInfo>& streams,
                                int& preferredIndex,
                                MusicVideoStreamInfo& videoStream,
                                bool& hasVideoStream);
};
