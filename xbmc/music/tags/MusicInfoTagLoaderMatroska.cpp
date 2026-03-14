/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicInfoTagLoaderMatroska.h"

#include "MusicInfoTag.h"
#include "ServiceBroker.h"
#include "cores/FFmpeg.h"
#include "filesystem/File.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <taglib/matroskafile.h>
#include <taglib/matroskatag.h>
#include <taglib/matroskasimpletag.h>
#include <taglib/matroskaattachments.h>
#include <taglib/matroskaattachedfile.h>
#include <taglib/matroskachapters.h>
#include <taglib/matroskachapteredition.h>
#include <map>
#include <vector>
#include <exception>

using namespace MUSIC_INFO;
using namespace XFILE;
using namespace TagLib;

static int vfs_file_read(void* h, uint8_t* buf, int size)
{
  CFile* pFile = static_cast<CFile*>(h);
  return pFile->Read(buf, size);
}

static int64_t vfs_file_seek(void* h, int64_t pos, int whence)
{
  CFile* pFile = static_cast<CFile*>(h);
  if (whence == AVSEEK_SIZE)
    return pFile->GetLength();
  else
    return pFile->Seek(pos, whence & ~AVSEEK_FORCE);
}

// Used by Matroka files with no chapters (most comon) or with a single (one song)
bool CMusicInfoTagLoaderMatroska::Load(const std::string& strFileName,
                                       CMusicInfoTag& tag,
                                       EmbeddedArt* art)
{
  tag.SetLoaded(false);

  CFile file;
  if (!file.Open(strFileName))
    return false;

  int bufferSize = 4096;
  int blockSize = file.GetChunkSize();
  if (blockSize > 1)
    bufferSize = blockSize;
  uint8_t* buffer = (uint8_t*)av_malloc(bufferSize);
  AVIOContext* ioctx =
      avio_alloc_context(buffer, bufferSize, 0, &file, vfs_file_read, NULL, vfs_file_seek);

  AVFormatContext* fctx = avformat_alloc_context();
  fctx->pb = ioctx;

  if (file.IoControl(IOCTRL_SEEK_POSSIBLE, NULL) != 1)
    ioctx->seekable = 0;

  const AVInputFormat* iformat = nullptr;
  av_probe_input_buffer(ioctx, &iformat, strFileName.c_str(), NULL, 0, 0);

  if (avformat_open_input(&fctx, strFileName.c_str(), iformat, NULL) < 0)
  {
    if (fctx)
      avformat_close_input(&fctx);
    av_free(ioctx->buffer);
    av_free(ioctx);
    return false;
  }

  std::vector<std::string> separators{";", " feat. ", " ft. ", " Feat. ", " Ft. ", ":",
                                      "|", "#",       "/",     " with ",  "&"};
  std::string musicsep = 
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;
  if (musicsep.find_first_of(";/,&|#") == std::string::npos)
    separators.push_back(musicsep);

  tag.SetDuration(fctx->duration * av_q2d(av_get_time_base_q()));
  
  avformat_close_input(&fctx);
  av_free(ioctx->buffer);
  av_free(ioctx);

  std::map<std::string, std::string> fileTags;
  std::map<unsigned long long, std::map<std::string, std::string>> chapterTags;
  std::vector<unsigned long long> chapterOrder;
  GetMatroskaMusicTags(strFileName, fileTags, chapterTags, chapterOrder);

  if (fileTags.empty())
    return true;
  for (const auto& t : fileTags)
    ParseTag(t.first, t.second, separators, musicsep, tag);
  /*!
  // now process the Chapter (track) if the Matroska file has a chapter
  // there is usually no chapters but there could be one, if > 1 
  // the file is processed as a whole album by CAuidoBookFileDirectory
  */
  if (!chapterOrder.empty())
  {
    auto it = chapterTags.find(chapterOrder[0]);
    if (it != chapterTags.end())
    {
      for (const auto& t : it->second)
        ParseTag(t.first, t.second, separators, musicsep, tag);
    }
  }

  if (!tag.GetAlbum().empty() || !tag.GetTitle().empty())
     tag.SetLoaded(true);

  return true;
}


void CMusicInfoTagLoaderMatroska::ParseTag(const std::string& key,
                                           const std::string& value,
                                           std::vector<std::string>& separators,
                                           const std::string& musicsep,
                                           CMusicInfoTag& tag)
{
  // Matroska Tag spec does not allow storing multi values in a single tag, but some tools
  // do it anyway using a separator. So we need to split the value using the separator and
  // then join it back using the music item separator from as.xml if needed. 
  if (key == "ALBUM")
    tag.SetAlbum(value);
  else if (key == "ARTIST")
  // tag.SetArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
    tag.SetArtist(value);
  else if (key == "ARTISTS")
    tag.SetMusicBrainzArtistHints(StringUtils::Split(value, separators));
  else if (key == "ALBUMARTISTS" || key == "ALBUMARTSTS" || key == "ALBUM ARTISTS")
    tag.SetAlbumArtist(value);
  else if (key == "ALBUM_ARTIST" || key == "ALBUM ARTIST" || key == "ALBUMARTIST")
    tag.SetAlbumArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "TITLE")
    tag.SetTitle(value);
  else if (key == "PART_NUMBER" || key == "TRACK")
    tag.SetTrackNumber(std::stoi(value));
  else if (key == "DISC" || key == "DISCNUMBER")
    tag.SetDiscNumber(std::stoi(value));
  else if (key == "COMPILATION")
    tag.SetCompilation(true);
  else if (key == "ENCODED_BY")
  {
  }
  else if (key == "LABEL" || key == "PUBLISHER")
    tag.SetRecordLabel(value);
  else if (key == "CATALOGNUMBER")
  {
  } // No database field yet
  else if (key == "COPYRIGHT")
  {
  } // Copyright message
  else if (key == "DATE" || key == "DATE_RELEASED" || key == "YEAR")
    tag.SetReleaseDate(value);
  else if (key == "DATE_RECORDED" || key == "ORIGINALDATE" || key == "ORIGINALYEAR" ||
           key == "ORIGYEAR")
    tag.SetOriginalDate(value);
  else if (key == "LANGUAGE")
  {
  } // Languages
  else if (key == "ARTIST-SORT" || key == "ARTISTSORT" || key == "ARTIST SORT")
    tag.SetArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "ALBUMARTISTSORT" || key == "ALBUM ARTIST SORT" || key == "SORT_ALBUM_ARTIST")
    tag.SetAlbumArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "COMPOSERSORT" || key == "COMPOSER SORT")
    tag.SetComposerSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "DISCSUBTITLE" || key == "SUBTITLE" || key == "SETSUBTITLE")
    tag.SetDiscSubtitle(value);
  else if (key == "MUSICBRAINZ ARTIST ID" || key == "MUSICBRAINZ_ARTISTID")
    tag.SetMusicBrainzArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ ALBUM ID" || key == "MUSICBRAINZ_ALBUMID")
    tag.SetMusicBrainzAlbumID(value);
  else if (key == "MUSICBRAINZ RELEASEGROUP ID" || key == "MUSICBRAINZ_RELEASEGROUPID" ||
           key == "MUSICBRAINZ RELEASE GROUP ID")
    tag.SetMusicBrainzReleaseGroupID(value);
  else if (key == "MUSICBRAINZ ALBUM ARTIST ID" || key == "MUSICBRAINZ_ALBUMARTISTID" ||
           key == "MUSICBRAINZ ALBUM ARTIST ID")
    tag.SetMusicBrainzAlbumArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ TRACKID" || key == "MUSICBRAINZ_TRACKID")
    tag.SetMusicBrainzTrackID(value);
  else if (key == "MUSICBRAINZ ALBUM ARTIST" || key == "MUSICBRAINZ_ALBUMARTIST")
    tag.SetAlbumArtist(value);
  else if (key == "MUSICBRAINZ ALBUM TYPE" || key == "MUSICBRAINZ_ALBUMTYPE")
    tag.SetMusicBrainzReleaseType(value);
  else if (key == "MUSICBRAINZ ALBUM STATUS" || key == "MUSICBRAINZ_ALBUMSTATUS")
    tag.SetAlbumReleaseStatus(value);
  else if (key == "MOOD")
    tag.SetMood(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  // genre could be comma delimited or not. Temporarily add the comma just in case.
  // true trims any whitespace around the genre(s)
  else if (key == "GENRE")
  {
    tag.SetGenre(StringUtils::Split(value, musicsep), true);
  }
  else if (key == "COMMENT")
    tag.SetComment(value);
  else if (key == "WRITER")
    tag.AddArtistRole("Writer", StringUtils::Split(value, separators));
  else if (key == "PERFORMER")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddRole(tagdata, separators, tag);
  }
  else if (key == "ARRANGER")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddRole(tagdata, separators, tag);
  }
  else if (key == "REMIXED_BY" || key == "REMIXEDBY")
    tag.AddArtistRole("Remixer", StringUtils::Split(value, separators));
  else if (key == "MIXED_BY" || key == "MIXER")
    tag.AddArtistRole("Mixer", StringUtils::Split(value, separators));
  else if (key == "LYRICIST")
    tag.AddArtistRole("Lyricist", StringUtils::Split(value, separators));
  else if (key == "COMPOSER")
    tag.AddArtistRole("Composer", StringUtils::Split(value, separators));
  else if (key == "CONDUCTOR")
    tag.AddArtistRole("Conductor", StringUtils::Split(value, separators));
  else if (key == "ENGINEER")
    tag.AddArtistRole("Engineer", StringUtils::Split(value, separators));
  else if (key == "PRODUCER")
    tag.AddArtistRole("Producer", StringUtils::Split(value, separators));
  else if (key == "BAND")
    tag.AddArtistRole("Band", StringUtils::Split(value, separators));
  // comma separated list of role, person
  else if (key == "INVOLVEDPEOPLE" || key == "ACTOR")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddCommaDelimitedString(tagdata, separators, tag);
  }
  else if (key == "INSTRUMENTS")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, separators);
    AddCommaDelimitedString(tagdata, separators, tag);
  }
}

void CMusicInfoTagLoaderMatroska::AddRole(const std::vector<std::string>& data,
                                          const std::vector<std::string>& separators,
                                          CMusicInfoTag& musictag)
{
  if (!data.empty())
  {
    for (size_t i = 0; i + 1 < data.size(); i += 2)
    {
      std::vector<std::string> roles = StringUtils::Split(data[i], separators);
      for (auto& role : roles)
      {
        StringUtils::Trim(role);
        StringUtils::ToCapitalize(role);
        musictag.AddArtistRole(role, StringUtils::Split(data[i + 1], separators));
      }
    }
  }
}

void CMusicInfoTagLoaderMatroska::AddCommaDelimitedString(
    const std::vector<std::string>& data,
    const std::vector<std::string>& separators,
    CMusicInfoTag& musictag)
{
  if (!data.empty())
  {
    for (size_t i = 0; i + 1 < data.size(); i += 2)
    {
      std::vector<std::string> roles = StringUtils::Split(data[i], separators);
      for (auto& role : roles)
      {
        StringUtils::Trim(role);
        StringUtils::ToCapitalize(role);
        musictag.AddArtistRole(role, StringUtils::Split(data[i + 1], ","));
      }
    }
  }
}

/*!
 * use TagLib to read hierarchy of tags in file and populate album and chapter
 * (track) tags. This creates a map of chapterUid to track tags for each chapter
*/
void CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(const std::string& fileName,
  std::map<std::string, std::string>& fileTags,
  std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
  std::vector<unsigned long long>& chapterOrder)
{
  fileTags.clear();
  chapterTags.clear();
  chapterOrder.clear();

  TagLib::Matroska::File* matroskaFile = nullptr;
  Matroska::Tag* matroskatag = nullptr;

  try
  {
    matroskaFile = new TagLib::Matroska::File(fileName.c_str());
    if (matroskaFile->isValid())
      matroskatag = matroskaFile->tag(false);
    if (!matroskatag)
    {
      delete matroskaFile;
      return;
    }

    int chapterCount = 0;
    /*!
    * first get all chapters and get the chapter name for each chapter and store
    * it in the albumtracktags map. Then we have chapter name for each chapter
    * (track) if Chapters are not tagged.
    */
    TagLib::Matroska::Chapters* chapters = matroskaFile->chapters();
    if (chapters)
    {
      const TagLib::Matroska::Chapters::ChapterEditionList& editions =
          chapters->chapterEditionList();
      for (const auto& edition : editions)
      {
        if (edition.uid())
        {
          for (const auto& chapter : edition.chapterList())
          {
            for (const auto& display : chapter.displayList())
            {
              std::map<std::string, std::string> chapterTagList = {
                  {"CHAPTERNAME", display.string().toCString(true)}};
              chapterTags[chapter.uid()] = chapterTagList;
            }
            chapterOrder.push_back(chapter.uid());
            chapterCount++;
          }
        }
      }

      /*!
      * read all simple tags and group them by file (album or song files with no
      * chapters) or by chapter/track (if target type value is 30).
      * Delimiter separated lists are outside the Matroska spec
      * (see https://www.matroska.org/technical/tagging.html) it states to use
      * multiple simple tags for eg 2 or more composers.To ensure Kodi can use
      * muliple same name tags need create a single tag with multiple values in
      * a semicolon delimitered string (Kodi handles multiple values with  
      * delimited strings).
      * 
      * Special handling for TITLE tag with target type value 50 which is the Album
      * tag type in Matroska tag spec to elminate the conflict with TITLE tag for tracks
      * Internally Kodi uses ALBUM. ALBUM tag may exist as some taggers used it
      * for compatibility with Kodi 21.3 (all due to a ffmpeg tag bug)
      */
      const TagLib::Matroska::SimpleTagsList& list = matroskatag->simpleTagsList();
      for (const TagLib::Matroska::SimpleTag& tag : list)
      {
        unsigned long long chapterUid;
        std::string upperName = tag.name().to8Bit(true);
        StringUtils::ToUpper(upperName);
        unsigned int targetTypeValue = static_cast<unsigned int>(tag.targetTypeValue());
        if (targetTypeValue == 0 || targetTypeValue == 50) 
        {
          if (upperName == "TITLE" && targetTypeValue == 50)
            upperName = "ALBUM";

          auto it = fileTags.find(upperName);
          if (it == fileTags.end())
          {
            fileTags[upperName] = tag.toString().to8Bit(true);
          }
          else
          {
            if (upperName != "ALBUM")
              it->second += ";" + tag.toString().to8Bit(true);
          }
        }
        else if (targetTypeValue == 30) 
        {
          chapterUid = tag.chapterUid();
          if (chapterUid > 0)
          {
            auto chapterIt = chapterTags.find(chapterUid);
            if (chapterIt == chapterTags.end())
            {
              std::map<std::string, std::string> chapterTagList;
              chapterTagList[upperName] = tag.toString().to8Bit(true);
              chapterTags[chapterUid] = chapterTagList;
            }
            else
            {
              auto& chapterTagList = chapterIt->second;
              auto tagIt = chapterTagList.find(upperName);
              if (tagIt == chapterTagList.end())
              {
                chapterTagList[upperName] = tag.toString().to8Bit(true);
              }
              else
              {
                tagIt->second += ";" + tag.toString().to8Bit(true);
              }
            }
          }
          else
          {
            auto it = fileTags.find(upperName);
            if (it == fileTags.end())
            {
              fileTags[upperName] = tag.toString().to8Bit(true);
            }
            else
            {
              if (chapterCount == 0)
              {
                it->second += ";" + tag.toString().to8Bit(true);
              }
            }
          }
        }
      }

      ///* DEBUG */
      //for (const auto& fileTag : fileTags)
      //{
      //  std::cout << "FILE TAG: " << fileTag.first << " = " << fileTag.second << std::endl;
      //}

      //int i = 0;
      //for (const auto& chapterTagList : chapterTags)
      //{
      //  i++;
      //  for (const auto& chapterTag : chapterTagList.second)
      //  {
      //    std::cout << "CHAPTER" << i << " TAG: " << chapterTag.first << " = " << chapterTag.second
      //              << std::endl;
      //  }
      //}
      

    }
    if (matroskaFile)
      delete matroskaFile;
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "GetMatroskaMusicTags: Exception while reading Matroska tags: {} {}", 
        fileName, e.what());
    if (matroskaFile)
      delete matroskaFile;
  }
}

