/*
 *  Copyright (C) 2014 Arne Morten Kvarving
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioBookFileDirectory.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "TextureDatabase.h"
#include "URL.h"
#include "Util.h"
#include "filesystem/File.h"
#include "cores/FFmpeg.h"
#include "guilib/LocalizeStrings.h"
#include "music/MusicDatabase.h"
#include "music/tags/KodiTagLibStream.h"
#include "music/MusicEmbeddedCoverLoaderFFmpeg.h"
#include "music/tags/MusicCodecInfoFFmpeg.h"
#include "music/tags/MusicInfoTagLoaderMatroska.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
//
//#include <taglib/mp4file.h>
//#include <taglib/mp4tag.h>
//#include <taglib/tpropertymap.h>
//#include <taglib/tvariant.h>

#include <map>
#include <tuple>
#include <vector>


using namespace XFILE;
using namespace MUSIC_INFO;

static int cfile_file_read(void* h, uint8_t* buf, int size)
{
  CFile* pFile = static_cast<CFile*>(h);
  return pFile->Read(buf, size);
}

static int64_t cfile_file_seek(void* h, int64_t pos, int whence)
{
  CFile* pFile = static_cast<CFile*>(h);
  if (whence == AVSEEK_SIZE)
    return pFile->GetLength();
  else
    return pFile->Seek(pos, whence & ~AVSEEK_FORCE);
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


bool CAudioBookFileDirectory::GetDirectory(const CURL& url, CFileItemList& items)
{
  if (!m_fctx && !ContainsFiles(url))
    return true;

  std::string title;
  std::string author;
  std::string album;
  std::string desc;

  std::vector<std::string> separators{" feat. ", " ft. ", " Feat. ", " Ft. ",  ";", ":",
                                      "|",       "#",     "/",       " with ", "&"};
  const std::string musicsep =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;
  if (musicsep.find_first_of(";/,&|#") == std::string::npos)
    separators.push_back(musicsep); // add custom music separator from as.xml

  // FIX: Guard streams[0] access — crash if file has no streams
  const int end_time_m4b_file = (m_fctx->nb_streams > 0) ? m_fctx->streams[0]->duration *
                                                               av_q2d(m_fctx->streams[0]->time_base)
                                                         : 0;

  const bool isAudioBook = url.IsFileType("m4b");
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
    CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(url.Get(), fileTags, chapterTags,
                                                      chapterOrder);
    if (fileTags.empty())
      return true;
    /*!
     * initially just get the (file) Album level tags to be use in subsequent tracks
     * (chapters) processed below to create Kodi music Songs
    */
    for (const auto& t : fileTags)
      CMusicInfoTagLoaderMatroska::ParseTag(t.first, t.second, separators, musicsep, albumtag);
  }

  std::string thumb;
  thumb = IMAGE_FILES::URLFromFile(url.Get(), "music");
  // Look for any embedded cover art
  CMusicEmbeddedCoverLoaderFFmpeg::GetEmbeddedCover(m_fctx, albumtag);

 // now get the AudioCodec etc for QQ Kodi-------------------------------------
  bool haveFFmpegInfo = false;
  musicCodecInfo codec_info;
  haveFFmpegInfo = CMusicCodecInfoFFmpeg::GetMusicCodecInfo(url.Get(), codec_info);
  if (haveFFmpegInfo) // use data from FFmpeg (taglib 2.2.1 does not support some codecs)
  {
    albumtag.SetBitRate(codec_info.bitRate);
    albumtag.SetSampleRate(codec_info.sampleRate);
    albumtag.SetBitsPerSample(codec_info.bitsPerSample);
    albumtag.SetCodec(codec_info.codecName);
    albumtag.SetNoOfChannels(codec_info.channels);
    albumtag.SetDuration(codec_info.duration);
  }

  float chapter_size = 0;
  bool chapter_error = false;
  for (unsigned int i = 0; i < m_fctx->nb_chapters; ++i)
  {
    if (m_fctx->chapters[i]->start < 0) // negative start time, ignore it
      continue;
    chapter_size = m_fctx->chapters[i]->end * av_q2d(m_fctx->chapters[i]->time_base);
    if (chapter_size < 1 && chapter_size > 0) // Chapter must have positive time of more than 1 sec
    {
      CLog::Log(LOGWARNING,
                "CAudioBookFileDirectory: Tiny chapter of size {}s detected when scanning {} Most "
                "likely this file needs the chapters correcting",
                chapter_size, url.GetRedacted());
      chapter_error = true;
      continue;
    }

    tag = nullptr;
    std::string chaptitle = StringUtils::Format(
        CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(25010), i + 1);
    std::string chapauthor;
    std::string chapalbum;

    std::shared_ptr<CFileItem> item(new CFileItem(url.Get(), false));
    *item->GetMusicInfoTag() = albumtag;
 
    if (isAudioBook)
    {
      while ((tag = av_dict_get(m_fctx->chapters[i]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
      {
        if (StringUtils::CompareNoCase(tag->key, "title") == 0)
           chaptitle = tag->value;
        else if (StringUtils::CompareNoCase(tag->key, "artist") == 0)
           chapauthor = tag->value;
        else if (StringUtils::CompareNoCase(tag->key, "album") == 0)
           chapalbum = tag->value;
      }
      item->GetMusicInfoTag()->SetTitle(chaptitle);
      item->GetMusicInfoTag()->SetAlbum(chapalbum.empty() ? album.empty() ? title : album
                                                          : chapalbum);
      item->GetMusicInfoTag()->SetArtist(chapauthor.empty() ? author : chapauthor);
      if (!desc.empty())
        item->GetMusicInfoTag()->SetComment(desc);
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
            CMusicInfoTagLoaderMatroska::ParseTag(Tracktag.first,
                                                   Tracktag.second,
                                                   separators,
                                                   musicsep,
                                                   *item->GetMusicInfoTag());

          item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(std::get<2>(chapterOrder[i])));
          item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(std::get<3>(chapterOrder[i])));
          item->GetMusicInfoTag()->SetDuration(
              CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));
        }
      }
    }
 
    item->GetMusicInfoTag()->SetTrackNumber(i + 1);
    item->GetMusicInfoTag()->SetLoaded(true);

    item->SetLabel(StringUtils::Format("{0:02}. {1} - {2}", i + 1,
                                         item->GetMusicInfoTag()->GetAlbum(),
                                         item->GetMusicInfoTag()->GetTitle()));
    //item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(m_fctx->chapters[i]->start *
    //                                                     av_q2d(m_fctx->chapters[i]->time_base)));
    //item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(m_fctx->chapters[i]->end *
    //                                                   av_q2d(m_fctx->chapters[i]->time_base)));
    //if (item->GetEndOffset() < 0 ||
    //    item->GetEndOffset() > CUtil::ConvertMilliSecsToSecs(m_fctx->duration))
    //{
    //  if (i < m_fctx->nb_chapters - 1)
    //    item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(
    //        m_fctx->chapters[i + 1]->start * av_q2d(m_fctx->chapters[i + 1]->time_base)));
    //  else
    //  {
    //    item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(end_time_mka_file)); // mka file
    //    if (item->GetEndOffset() < 0)
    //      item->SetEndOffset(CUtil::ConvertSecsToMilliSecs(end_time_m4b_file)); // m4b file
    //  }
    //}
    //item->GetMusicInfoTag()->SetDuration(
    //    CUtil::ConvertMilliSecsToSecsInt(item->GetEndOffset() - item->GetStartOffset()));

    item->SetProperty("item_start", item->GetStartOffset());
    item->SetProperty("audio_bookmark", item->GetStartOffset());
    if (!thumb.empty() && !chapter_error)
      item->SetArt("thumb", thumb);
    items.Add(item);
  }
  return true;
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

  // DB unavailable (-1). Return false to avoid blocking playback.
  // The file will be properly detected during library scan when the DB
  // is available. Returning true here risks triggering GetDirectory()
  // which opens more file/DB handles and can deadlock during playback.
  return false;
}

bool CAudioBookFileDirectory::HasChaptersInDatabase(const CURL& url)
{
  return Exists(url);
}


bool CAudioBookFileDirectory::ContainsFiles(const CURL& url)
{
  CFile file;
  if (!file.Open(url))
    return false;

  uint8_t* buffer = (uint8_t*)av_malloc(32768);
  m_ioctx = avio_alloc_context(buffer, 32768, 0, &file, cfile_file_read, nullptr, cfile_file_seek);

  m_fctx = avformat_alloc_context();
  m_fctx->pb = m_ioctx;
  m_fctx->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (file.IoControl(IOCTRL_SEEK_POSSIBLE, nullptr) == 0)
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


//bool CAudioBookFileDirectory::ContainsFiles(const CURL& url)
//{
//  // Fast path: check the music database first — avoids opening the file entirely
//  // when it has already been scanned into the library
//  int dbSongCount = GetSongCountFromDatabase(url);
//  if (dbSongCount > 1)
//  {
//    CLog::Log(LOGDEBUG, "CAudioBookFileDirectory::ContainsFiles: DB fast path — {} songs for {}",
//              dbSongCount, url.GetRedacted());
//    return true;
//  }
//  else if (dbSongCount == 1)
//  {
//    CLog::Log(LOGDEBUG,
//              "CAudioBookFileDirectory::ContainsFiles: DB fast path — single song for {}",
//              url.GetRedacted());
//    return false;
//  }
//
//  // Slow path: file not in database — open via FFmpeg and check chapter count.
//  // m_fctx is kept open for GetDirectory() to reuse for codec info.
//  if (!m_file.Open(url))
//    return false;
//
//  uint8_t* buffer = (uint8_t*)av_malloc(32768);
//  m_ioctx = avio_alloc_context(buffer, 32768, 0, &m_file, cfile_file_read, nullptr, cfile_file_seek);
//
//  m_fctx = avformat_alloc_context();
//  m_fctx->pb = m_ioctx;
//  m_fctx->flags |= AVFMT_FLAG_CUSTOM_IO;
//
//  if (m_file.IoControl(IOCTRL_SEEK_POSSIBLE, nullptr) == 0)
//    m_ioctx->seekable = 0;
//
//  m_ioctx->max_packet_size = 32768;
//
//  const AVInputFormat* iformat = nullptr;
//  av_probe_input_buffer(m_ioctx, &iformat, url.Get().c_str(), nullptr, 0, 0);
//
//  bool contains = false;
//
//  if (avformat_open_input(&m_fctx, url.Get().c_str(), iformat, nullptr) < 0)
//  {
//    if (m_fctx)
//      avformat_close_input(&m_fctx);
//    av_free(m_ioctx->buffer);
//    av_free(m_ioctx);
//    return false;
//  }
//  m_fctx->flags |= AVFMT_FLAG_NOPARSE;
//  int err = avformat_find_stream_info(m_fctx, NULL);
//  if (err < 0)
//    CLog::Log(LOGERROR, "Can't detect codec info in file {}", url.GetRedacted());
//
//  contains = m_fctx->nb_chapters > 1;
//
//  return contains;
//}
