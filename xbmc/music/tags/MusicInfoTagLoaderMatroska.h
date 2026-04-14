/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ImusicInfoTagLoader.h"
#include "KodiTagLibStream.h"

#include <map>
#include <string>
#include <tuple>
#include <vector>

struct AVFormatContext;

namespace MUSIC_INFO
{
class CMusicInfoTagLoaderMatroska : public IMusicInfoTagLoader
{
public:
  CMusicInfoTagLoaderMatroska() = default;
  ~CMusicInfoTagLoaderMatroska() override = default;

  bool Load(const std::string& strFileName,
            CMusicInfoTag& tag,
            EmbeddedArt* art = nullptr) override;

  static void ParseTag(const std::string& key,
                       const std::string& value,
                       std::vector<std::string>& separators,
                       const std::string& musicsep,
                       CMusicInfoTag& tag);

  // Static overload for external callers (e.g. AudioBookFileDirectory) —
  // opens its own KodiTagLibStream internally.
  // If coverTag is non-null, embedded cover art info is set on it.
  static void GetMatroskaMusicTags(
      const std::string& fileName,
      std::map<std::string, std::string>& fileTags,
      std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
      std::vector<std::tuple<unsigned long long, std::string, double, double>>& chapterOrder,
      CMusicInfoTag* coverTag = nullptr);

  /*!
   * \Read audio codec properties from an FFmpeg format context into a music info tag.
   * \param fctx An open AVFormatContext with stream info already detected.
   * \param tag  CMusicInfoTag to receive the audio properties.
   */
  static void SetAudioPropertiesFromFFmpeg(AVFormatContext* fctx, CMusicInfoTag& tag);

private:
  // Internal overload used by Load() — reuses an already-open stream
  static void GetMatroskaMusicTags(
      const std::string& fileName,
      KodiTagLibStream& matroskaStream,
      std::map<std::string, std::string>& fileTags,
      std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
      std::vector<std::tuple<unsigned long long, std::string, double, double>>& chapterOrder,
      CMusicInfoTag* coverTag = nullptr,
      EmbeddedArt* art = nullptr);

  static void AddRole(const std::vector<std::string>& data,
                      const std::vector<std::string>& separators,
                      CMusicInfoTag& musictag);
  static void AddCommaDelimitedString(const std::vector<std::string>& data,
                                      const std::vector<std::string>& separators,
                                      CMusicInfoTag& musictag);
};
} 