/*
 *  Copyright (C) 2014 Arne Morten Kvarving
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioBookFileDirectory.h"

#include "FileItem.h"
#include "TextureDatabase.h"
#include "URL.h"
#include "Util.h"
#include "cores/FFmpeg.h"
#include "guilib/LocalizeStrings.h"
#include "music/MusicDatabase.h"
#include "music/tags/KodiTagLibStream.h"
#include "music/tags/MusicInfoTagLoaderMatroska.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "utils/StringUtils.h"

#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#include <map>
#include <tuple>
#include <vector>


using namespace XFILE;
using namespace MUSIC_INFO;

static int cfile_file_read(void *h, uint8_t* buf, int size)
{
  CFile* pFile = static_cast<CFile*>(h);
  return pFile->Read(buf, size);
}

static int64_t cfile_file_seek(void *h, int64_t pos, int whence)
{
  CFile* pFile = static_cast<CFile*>(h);
  if(whence == AVSEEK_SIZE)
    return pFile->GetLength();
  else
    return pFile->Seek(pos, whence & ~AVSEEK_FORCE);
}

/*!
 * \brief Chapter data extracted from an MP4/M4B file via TagLib.
 */
struct Mp4Chapter
{
  std::string title;
  double startSecs{0.0};
  double endSecs{0.0};
};

/*!
 * \brief Embedded cover art info extracted from an MP4/M4B file via TagLib.
 */
struct Mp4CoverArt
{
  bool found{false};
  size_t size{0};
  std::string mimeType;
};

/*!
 * \brief Read M4B chapters and embedded cover art using TagLib.
 *
 * Chapters are read via complexProperties("CHAPTER"), each entry having:
 *   "TITLE"      — chapter title (String)
 *   "START_TIME" — start time in milliseconds (LongLong)
 *   "END_TIME"   — end time in milliseconds (LongLong)
 *
 * Cover art is read via complexProperties("PICTURE"), each entry having:
 *   "data"       — image data (ByteVector)
 *   "mimeType"   — MIME type string (String)
 *
 * \param fileName  Path to the M4B file (VFS-safe).
 * \param[out] chapters  Vector of {title, startSecs, endSecs} tuples.
 * \param[out] coverArt  Embedded cover art info (size + MIME type).
 * \return true if the file was opened and parsed successfully.
 */
static bool ReadMp4TagLib(const std::string& fileName,
                          std::vector<Mp4Chapter>& chapters,
                          Mp4CoverArt& coverArt)
{
  chapters.clear();
  coverArt = {};

  KodiTagLibStream stream(fileName);
  if (!stream.open())
    return false;

  TagLib::MP4::File mp4File(&stream);
  if (!mp4File.isValid())
    return false;

  TagLib::MP4::Tag* mp4tag = mp4File.tag();
  if (!mp4tag)
    return false;

  // --- Read chapters ---
  auto chapterList = mp4tag->complexProperties("CHAPTER");
  if (!chapterList.isEmpty())
  {
    for (const auto& chapterMap : chapterList)
    {
      Mp4Chapter ch;

      auto titleIt = chapterMap.find("TITLE");
      if (titleIt != chapterMap.end())
        ch.title = titleIt->second.toString().toCString(true);

      auto startIt = chapterMap.find("START_TIME");
      if (startIt != chapterMap.end())
        ch.startSecs = static_cast<double>(startIt->second.toLongLong()) / 1000.0;

      auto endIt = chapterMap.find("END_TIME");
      if (endIt != chapterMap.end())
        ch.endSecs = static_cast<double>(endIt->second.toLongLong()) / 1000.0;

      chapters.push_back(ch);
    }

    // If TagLib didn't provide end times, compute them from the next chapter's start
    for (size_t i = 0; i + 1 < chapters.size(); ++i)
    {
      if (chapters[i].endSecs <= 0.0)
        chapters[i].endSecs = chapters[i + 1].startSecs;
    }

    CLog::Log(LOGDEBUG, "ReadMp4TagLib: found {} chapters via TagLib for {}", chapters.size(),
               fileName);
  }
  else
  {
    CLog::Log(LOGDEBUG, "ReadMp4TagLib: no chapters found via TagLib for {}", fileName);
  }

  // --- Read embedded cover art ---
  auto pictureList = mp4tag->complexProperties("PICTURE");
  if (!pictureList.isEmpty())
  {
    const auto& pictureMap = pictureList.front();

    auto dataIt = pictureMap.find("data");
    auto mimeIt = pictureMap.find("mimeType");

    if (dataIt != pictureMap.end())
    {
      coverArt.size = dataIt->second.toByteVector().size();

      if (mimeIt != pictureMap.end())
        coverArt.mimeType = mimeIt->second.toString().toCString(true);
      else
      {
        // Fallback: infer MIME type from the data if not provided
        coverArt.mimeType = "image/jpeg";
      }

      if (coverArt.size > 0)
      {
        coverArt.found = true;
        CLog::Log(LOGDEBUG, "ReadMp4TagLib: found embedded cover art ({} bytes, {}) for {}",
                   coverArt.size, coverArt.mimeType, fileName);
      }
    }
  }

  return true;
}

CAudioBookFileDirectory::~CAudioBookFileDirectory(void)
{
  if (m_fctx)
    avformat_close_input(&m_fctx);
  if (m_ioctx)
  {
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
  }
}

bool CAudioBookFileDirectory::EnsureFFmpegContext(const CURL& url)
{
  if (m_fctx)
    return true; // already open

  if (!m_file.Open(url))
    return false;

  uint8_t* buffer = (uint8_t*)av_malloc(32768);
  m_ioctx = avio_alloc_context(buffer, 32768, 0, &m_file, cfile_file_read, nullptr, cfile_file_seek);

  m_fctx = avformat_alloc_context();
  m_fctx->pb = m_ioctx;
  m_fctx->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (m_file.IoControl(IOCTRL_SEEK_POSSIBLE, nullptr) == 0)
    m_ioctx->seekable = 0;

  m_ioctx->max_packet_size = 32768;

  const AVInputFormat* iformat = nullptr;
  av_probe_input_buffer(m_ioctx, &iformat, url.Get().c_str(), nullptr, 0, 0);

  if (avformat_open_input(&m_fctx, url.Get().c_str(), iformat, nullptr) < 0)
  {
    if (m_fctx)
      avformat_close_input(&m_fctx);
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
    m_ioctx = nullptr;
    return false;
  }
  m_fctx->flags |= AVFMT_FLAG_NOPARSE;
  int err = avformat_find_stream_info(m_fctx, NULL);
  if (err < 0)
    CLog::Log(LOGERROR, "Can't detect codec info in file {}", url.GetRedacted());

  return true;
}

bool CAudioBookFileDirectory::GetDirectory(const CURL& url, CFileItemList& items)
{
  if (!m_fctx && !ContainsFiles(url))
    return true;

  // Ensure FFmpeg context is available for codec info — may not be open
  // if ContainsFiles() used the DB fast path
  if (!EnsureFFmpegContext(url))
    return true;

  std::string title;
  std::string author;
  std::string album;
  std::string desc;

  std::vector<std::string> separators{" feat. ", " ft. ", " Feat. ", " Ft. ",  ";", ":",
                                      "|", "#", "/", " with ", "&"};
  const std::string musicsep =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;
  if (musicsep.find_first_of(";/,&|#") == std::string::npos)
    separators.push_back(musicsep); // add custom music separator from as.xml

  const bool isAudioBook = url.IsFileType("m4b");

  // For M4B files, read chapters and cover art via TagLib (no FFmpeg needed)
  std::vector<Mp4Chapter> mp4Chapters;
  Mp4CoverArt mp4CoverArt;
  if (isAudioBook)
  {
    ReadMp4TagLib(url.Get(), mp4Chapters, mp4CoverArt);

    // Set the last chapter's end time from the FFmpeg stream duration if needed
    if (!mp4Chapters.empty() && mp4Chapters.back().endSecs <= 0.0)
    {
      double endTimeSecs = 0.0;
      if (m_fctx->nb_streams > 0)
        endTimeSecs = m_fctx->streams[0]->duration * av_q2d(m_fctx->streams[0]->time_base);
      mp4Chapters.back().endSecs = endTimeSecs;
    }
  }

  // Some tags are relevant to the whole album - these are read first
  CMusicInfoTag albumtag;

  AVDictionaryEntry* tag = nullptr;
  if (isAudioBook)
  {
    while ((tag = av_dict_get(m_fctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
      if (StringUtils::CompareNoCase(tag->key, "title") == 0)
        title = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "album") == 0)
        album = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "artist") == 0)
        author = tag->value;
      else if (StringUtils::CompareNoCase(tag->key, "description") == 0)
        desc = tag->value;
    }
  }

  std::map<std::string, std::string> fileTags;
  std::map<unsigned long long, std::map<std::string, std::string>> chapterTags;
  std::vector<std::tuple<unsigned long long, std::string, double, double>> chapterOrder;
  if (!isAudioBook)
  {
    // Pass &albumtag so GetMatroskaMusicTags reads embedded cover art too
    CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(url.Get(), fileTags, chapterTags,
                                                      chapterOrder, &albumtag);
    if (fileTags.empty())
      return true;
    /*!
     * initially just get the (file) Album level tags to be use in subsequent tracks
     * (chapters) processed below to create Kodi music Songs
    */
    for (const auto& t : fileTags)
      CMusicInfoTagLoaderMatroska::ParseTag(t.first, t.second, separators, musicsep, albumtag);
  }

  // Determine chapter count — from TagLib for both M4B and Matroska
  const unsigned int chapterCount =
      isAudioBook ? static_cast<unsigned int>(mp4Chapters.size())
                  : static_cast<unsigned int>(chapterOrder.size());

  std::string thumb;
  if (chapterCount > 1)
    thumb = CTextureUtils::GetWrappedImageURL(url.Get(), "music");

  // Embedded cover art — TagLib for M4B (already read above),
  // TagLib for Matroska (read inside GetMatroskaMusicTags above)
  if (isAudioBook && mp4CoverArt.found)
    albumtag.SetCoverArtInfo(mp4CoverArt.size, mp4CoverArt.mimeType);

  // now get the AudioCodec etc
  AVStream* st = nullptr;
  std::string codec_name = "unknown";
  int streamIndex = -1;
  // Look for the default audio stream first
  for (unsigned int i = 0; i < m_fctx->nb_streams; ++i)
  {
    if (m_fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      if (m_fctx->streams[i]->disposition & AV_DISPOSITION_DEFAULT)
      {
        streamIndex = i;
        break;
      }
    }
  }
  // If no default stream was found, look for the first audio stream
  if (streamIndex == -1)
  {
    for (unsigned int i = 0; i < m_fctx->nb_streams; ++i)
    {
      if (m_fctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      {
        streamIndex = i;
        break;
      }
    }
  }
  if (streamIndex > -1)
  {
    st = m_fctx->streams[streamIndex];
    albumtag.SetBitsPerSample(st->codecpar->bits_per_coded_sample);
    albumtag.SetSampleRate(st->codecpar->sample_rate);
    albumtag.SetBitRate(st->codecpar->bit_rate);
    albumtag.SetNoOfChannels(st->codecpar->ch_layout.nb_channels);
    codec_name = avcodec_get_name(st->codecpar->codec_id);
    int par_profile = st->codecpar->profile;
    if (st->codecpar->codec_id == AV_CODEC_ID_DTS)
    {
      switch (par_profile)
      {
        case FF_PROFILE_DTS_HD_MA:
          codec_name = "dtshd_ma";
          break;
        case FF_PROFILE_DTS_96_24:
          codec_name = "dts_96_24";
          break;
        case FF_PROFILE_DTS_HD_MA_X:
          codec_name = "dtshd_ma_x";
          break;
        case FF_PROFILE_DTS_HD_MA_X_IMAX:
          codec_name = "dtshd_ma_x_imax";
          break;
        case FF_PROFILE_DTS_ES:
          codec_name = "dts_es";
          break;
        case FF_PROFILE_DTS_HD_HRA:
          codec_name = "dtshd_hra";
          break;
        case FF_PROFILE_DTS_EXPRESS:
          codec_name = "dts_express";
          break;
        default:
          codec_name = "dca";
          break;
      }
    }
    if (st->codecpar->codec_id == AV_CODEC_ID_EAC3 && par_profile == FF_PROFILE_EAC3_DDP_ATMOS)
      codec_name = "eac3_ddp_atmos";

    if (st->codecpar->codec_id == AV_CODEC_ID_TRUEHD && par_profile == FF_PROFILE_TRUEHD_ATMOS)
      codec_name = "truehd_atmos";
    albumtag.SetCodec(codec_name);
  }

  bool chapter_error = false;
  for (unsigned int i = 0; i < chapterCount; ++i)
  {
    double chapterStartSecs;
    double chapterEndSecs;
    std::string chaptitle = StringUtils::Format(g_localizeStrings.Get(25010), i + 1);

    if (isAudioBook)
    {
      // Chapter data comes from TagLib (MP4)
      chapterStartSecs = mp4Chapters[i].startSecs;
      chapterEndSecs = mp4Chapters[i].endSecs;

      if (!mp4Chapters[i].title.empty())
        chaptitle = mp4Chapters[i].title;
    }
    else
    {
      // Chapter data comes from TagLib (Matroska) via chapterOrder
      chapterStartSecs = std::get<2>(chapterOrder[i]);
      chapterEndSecs = std::get<3>(chapterOrder[i]);

      if (!std::get<1>(chapterOrder[i]).empty())
        chaptitle = std::get<1>(chapterOrder[i]);
    }

    // FIX: Check chapter duration (end - start), not just end time.
    // A tiny chapter at the 2-hour mark would have a large end time and pass
    // the old filter. Checking duration catches it correctly.
    double chapterDuration = chapterEndSecs - chapterStartSecs;
    if (chapterDuration > 0 && chapterDuration < 1.0)
    {
      CLog::Log(LOGWARNING,
                "CAudioBookFileDirectory: Tiny chapter of duration {}s detected when scanning {} "
                "Most likely this file needs the chapters correcting",
                chapterDuration, url.GetRedacted());
      chapter_error = true;
      continue;
    }

    std::shared_ptr<CFileItem> item(new CFileItem(url.Get(), false));
    *item->GetMusicInfoTag() = albumtag;

    if (isAudioBook)
    {
      item->GetMusicInfoTag()->SetTitle(chaptitle);
      item->GetMusicInfoTag()->SetAlbum(album.empty() ? title : album);
      item->GetMusicInfoTag()->SetArtist(author);
      if (!desc.empty())
        item->GetMusicInfoTag()->SetComment(desc);

      item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(chapterStartSecs));
      int64_t endOffset;
      if (chapterEndSecs > 0.0)
      {
        endOffset = CUtil::ConvertSecsToMilliSecs(chapterEndSecs);
      }
      else if (i + 1 < mp4Chapters.size())
      {
        endOffset = CUtil::ConvertSecsToMilliSecs(mp4Chapters[i + 1].startSecs);
      }
      else
      {
        // Fallback: use stream duration from FFmpeg
        double endTimeSecs = 0.0;
        if (m_fctx->nb_streams > 0)
          endTimeSecs = m_fctx->streams[0]->duration * av_q2d(m_fctx->streams[0]->time_base);
        endOffset = CUtil::ConvertSecsToMilliSecs(endTimeSecs);
      }
      item->SetEndOffset(endOffset);
      item->GetMusicInfoTag()->SetDuration(
          CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));
    }
    else
    {
      // process chapter tags for this track using file-order chapter UID
      if (i < chapterOrder.size())
      {
        auto it = chapterTags.find(std::get<0>(chapterOrder[i]));
        if (it != chapterTags.end())
        {
          for (const auto& Tracktag : it->second)
            CMusicInfoTagLoaderMatroska::ParseTag(Tracktag.first, Tracktag.second, separators,
                                                  musicsep, *item->GetMusicInfoTag());
        }
      }

      item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(chapterStartSecs));
      item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(chapterEndSecs));
      item->GetMusicInfoTag()->SetDuration(
          CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));
    }

    item->GetMusicInfoTag()->SetTrackNumber(i + 1);
    item->GetMusicInfoTag()->SetLoaded(true);

    item->SetLabel(StringUtils::Format("{0:02}. {1} - {2}", i + 1,
                                       item->GetMusicInfoTag()->GetAlbum(),
                                       item->GetMusicInfoTag()->GetTitle()));

    item->SetProperty("item_start", item->GetStartOffset());
    item->SetProperty("audio_bookmark", item->GetStartOffset());
    if (!thumb.empty() && !chapter_error)
      item->SetArt("thumb", thumb);
    items.Add(item);
  }
  return true;
}

bool CAudioBookFileDirectory::Exists(const CURL& url)
{
  // Fast path: check the music database to avoid any file I/O.
  // During playback this is called frequently (e.g. from IsAudioBook()),
  // so it must be fast and must not open the actual media file.
  int dbSongCount = GetSongCountFromDatabase(url);
  if (dbSongCount > 1)
    return true;
  if (dbSongCount >= 0)
    return false; // 0 or 1 songs — not a multi-chapter audiobook

  // DB unavailable (-1). Avoid expensive ContainsFiles() — just check
  // the file extension. Full chapter detection happens in GetDirectory().
  return url.IsFileType("m4b") || url.IsFileType("mka") || url.IsFileType("mkv");
}

int CAudioBookFileDirectory::GetSongCountFromDatabase(const CURL& url)
{
  CMusicDatabase db;
  if (!db.Open())
    return -1;

  std::string strPath = URIUtils::GetDirectory(url.Get());
  std::string strFileName = URIUtils::GetFileName(url.Get());

  std::string sql = db.PrepareSQL("SELECT COUNT(*) FROM song "
                                  "JOIN path ON song.idPath = path.idPath "
                                  "WHERE path.strPath = '%s' AND song.strFileName = '%s'",
                                  strPath.c_str(), strFileName.c_str());

  int count = db.GetSingleValueInt(sql);
  db.Close();

  return (count >= 0) ? count : -1;
}

bool CAudioBookFileDirectory::ContainsFiles(const CURL& url)
{
  // Fast path: check the music database first — avoids opening the file entirely
  // when it has already been scanned into the library
  int dbSongCount = GetSongCountFromDatabase(url);
  if (dbSongCount > 1)
  {
    CLog::Log(LOGDEBUG, "CAudioBookFileDirectory::ContainsFiles: DB fast path — {} songs for {}",
              dbSongCount, url.GetRedacted());
    return true;
  }
  else if (dbSongCount == 1)
  {
    CLog::Log(LOGDEBUG,
              "CAudioBookFileDirectory::ContainsFiles: DB fast path — single song for {}",
              url.GetRedacted());
    return false;
  }

  // Slow path: file not in database — open via FFmpeg and check chapter count.
  // m_fctx is kept open for GetDirectory() to reuse for codec info.
  if (!m_file.Open(url))
    return false;

  uint8_t* buffer = (uint8_t*)av_malloc(32768);
  m_ioctx = avio_alloc_context(buffer, 32768, 0, &m_file, cfile_file_read, nullptr, cfile_file_seek);

  m_fctx = avformat_alloc_context();
  m_fctx->pb = m_ioctx;
  m_fctx->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (m_file.IoControl(IOCTRL_SEEK_POSSIBLE, nullptr) == 0)
    m_ioctx->seekable = 0;

  m_ioctx->max_packet_size = 32768;

  const AVInputFormat* iformat = nullptr;
  av_probe_input_buffer(m_ioctx, &iformat, url.Get().c_str(), nullptr, 0, 0);

  bool contains = false;

  if (avformat_open_input(&m_fctx, url.Get().c_str(), iformat, nullptr) < 0)
  {
    if (m_fctx)
      avformat_close_input(&m_fctx);
    av_free(m_ioctx->buffer);
    av_free(m_ioctx);
    return false;
  }
  m_fctx->flags |= AVFMT_FLAG_NOPARSE;
  int err = avformat_find_stream_info(m_fctx, NULL);
  if (err < 0)
    CLog::Log(LOGERROR, "Can't detect codec info in file {}", url.GetRedacted());

  contains = m_fctx->nb_chapters > 1;

  return contains;
}
