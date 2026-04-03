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
#include "filesystem/File.h"
#include "guilib/LocalizeStrings.h"
#include "music/MusicEmbeddedCoverLoaderFFmpeg.h"
#include "music/tags/MusicInfoTagLoaderMatroska.h"
#include "music/tags/MusicInfoTag.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
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
  if (m_fctx->nb_chapters > 1)
    thumb = CTextureUtils::GetWrappedImageURL(url.Get(), "music");

  // Look for any embedded cover art
  CMusicEmbeddedCoverLoaderFFmpeg::GetEmbeddedCover(m_fctx, albumtag);

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
  for (unsigned int i = 0; i < m_fctx->nb_chapters; ++i)
  {
    if (m_fctx->chapters[i]->start < 0) // negative start time, ignore it
      continue;

    // FIX: Check chapter duration (end - start), not just end time.
    // A tiny chapter at the 2-hour mark would have a large end time and pass
    // the old filter. Checking duration catches it correctly.
    double chapterStartSecs = m_fctx->chapters[i]->start * av_q2d(m_fctx->chapters[i]->time_base);
    double chapterEndSecs = m_fctx->chapters[i]->end * av_q2d(m_fctx->chapters[i]->time_base);
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

    tag = nullptr;
    std::string chaptitle = StringUtils::Format(g_localizeStrings.Get(25010), i + 1);
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

      // FIX: Restore start/end offsets and duration for m4b chapters.
      // This was lost when the old shared offset code was commented out.
      item->SetStartOffset(CUtil::ConvertSecsToMilliSecs(chapterStartSecs));
      int64_t endOffset;
      if (m_fctx->chapters[i]->end > 0)
      {
        endOffset = CUtil::ConvertSecsToMilliSecs(chapterEndSecs);
      }
      else if (i + 1 < m_fctx->nb_chapters)
      {
        endOffset = CUtil::ConvertSecsToMilliSecs(m_fctx->chapters[i + 1]->start *
                                                  av_q2d(m_fctx->chapters[i + 1]->time_base));
      }
      else
      {
        endOffset = CUtil::ConvertSecsToMilliSecs(end_time_m4b_file);
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
  return CFile::Exists(url) && ContainsFiles(url);
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
