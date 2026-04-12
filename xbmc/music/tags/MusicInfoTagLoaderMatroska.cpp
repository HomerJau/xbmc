/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicInfoTagLoaderMatroska.h"

#include "KodiTagLibStream.h"
#include "MusicInfoTag.h"
#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/EmbeddedArt.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#ifdef TARGET_WINDOWS
#include "platform/win32/CharsetConverter.h"
#endif
#include <array>
#include <exception>
#include <map>
#include <tuple>
#include <vector>

#include <taglib/audioproperties.h>
#include <taglib/matroskaattachedfile.h>
#include <taglib/matroskaattachments.h>
#include <taglib/matroskachapteredition.h>
#include <taglib/matroskachapters.h>
#include <taglib/matroskafile.h>
#include <taglib/matroskasimpletag.h>
#include <taglib/matroskatag.h>
#include <taglib/tfilestream.h>
#include <taglib/tiostream.h>

using namespace MUSIC_INFO;
using namespace XFILE;
using namespace TagLib;

/*!
 * \brief Read embedded cover art from a Matroska file's attachments via TagLib.
 *
 * Matroska stores cover art as attached files. This searches for the first
 * attachment with a supported image MIME type (image/jpeg, image/png, image/bmp).
 *
 * \param matroskaFile  An open, valid TagLib Matroska::File.
 * \param tag           CMusicInfoTag to set cover art info on.
 * \param art           Optional EmbeddedArt to receive the actual image data.
 */
static void GetMatroskaEmbeddedCover(TagLib::Matroska::File& matroskaFile,
                                     CMusicInfoTag& tag,
                                     EmbeddedArt* art = nullptr) static void GetMatroskaEmbeddedCover(TagLib::Matroska::File& matroskaFile,
                                                      CMusicInfoTag& tag,
                                                      EmbeddedArt* art = nullptr)
{
  TagLib::Matroska::Attachments* attachments = matroskaFile.attachments();
  if (!attachments)
    return;

  const auto& attachedFiles = attachments->attachedFileList();
  for (const auto& file : attachedFiles)
  {
    std::string mimeType = file.mediaType().toCString(true);
    if (mimeType == "image/jpeg" || mimeType == "image/png" || mimeType == "image/bmp")
    {
      const TagLib::ByteVector& data = file.data();
      if (data.isEmpty())
        continue;

      tag.SetCoverArtInfo(data.size(), mimeType);
      if (art)
        art->Set(reinterpret_cast<const uint8_t*>(data.data()), data.size(), mimeType, "thumb");
      break; // just need one cover
    }
  }
}

// Used by Matroka files with no chapters (most comon) or with a single (one song)
bool CMusicInfoTagLoaderMatroska::Load(const std::string& strFileName,
                                       CMusicInfoTag& tag,
                                       EmbeddedArt* art)
{
  tag.SetLoaded(false);

  KodiTagLibStream matroskaStream(strFileName);
  if (!matroskaStream.open())
    return false;

  std::vector<std::string> separators{";", " feat. ", " ft. ", " Feat. ", " Ft. ", ":",
                                      "|", "#", "/", " with ", "&"};
  std::string musicsep =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;
  if (musicsep.find_first_of(";/,&|#") == std::string::npos)
    separators.push_back(musicsep);

  // Get tags, chapters, embedded cover art, and duration in one call
  // (single file parse — avoids opening the Matroska file twice)
  std::map<std::string, std::string> fileTags;
  std::map<unsigned long long, std::map<std::string, std::string>> chapterTags;
  std::vector<std::tuple<unsigned long long, std::string, double, double>> chapterOrder;
  GetMatroskaMusicTags(strFileName, matroskaStream, fileTags, chapterTags, chapterOrder, &tag, art);

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
    auto it = chapterTags.find(std::get<0>(chapterOrder[0]));
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
  else if (key == "ALBUMARTISTS" || key == "ALBUM_ARTISTS")
    tag.SetAlbumArtist(value);
  else if (key == "ALBUMARTIST" || key == "ALBUM_ARTIST")
    tag.SetAlbumArtist(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "TITLE")
    tag.SetTitle(value);
  else if (key == "PART_NUMBER" || key == "TRACK")
  {
    try
    {
      tag.SetTrackNumber(std::stoi(value));
    }
    catch (const std::exception&)
    {
    }
  }
  else if (key == "DISC" || key == "DISCNUMBER")
  {
    try
    {
      tag.SetDiscNumber(std::stoi(value));
    }
    catch (const std::exception&)
    {
    }
  }
  else if (key == "GENRE")
    tag.SetGenre(StringUtils::Split(value, musicsep), true);
  else if (key == "COMPILATION")
    tag.SetCompilation(true);
  else if (key == "DATE" || key == "DATE_RELEASED" || key == "YEAR")
    tag.SetReleaseDate(value);
  else if (key == "DATE_RECORDED" || key == "ORIGINALDATE" || key == "ORIGINALYEAR" ||
           key == "ORIGYEAR")
    tag.SetOriginalDate(value);
  else if (key == "MOOD")
    tag.SetMood(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  // genre could be comma delimited or not. Temporarily add the comma just in case.
  // true trims any whitespace around the genre(s)
  else if (key == "COMMENT")
    tag.SetComment(value);
  else if (key == "ARTIST-SORT" || key == "ARTISTSORT")
    tag.SetArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "ALBUMARTISTSORT" || key == "SORT_ALBUM_ARTIST")
    tag.SetAlbumArtistSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "COMPOSERSORT")
    tag.SetComposerSort(StringUtils::Join(StringUtils::Split(value, separators), musicsep));
  else if (key == "DISCSUBTITLE" || key == "SUBTITLE" || key == "SETSUBTITLE")
    tag.SetDiscSubtitle(value);
  else if (key == "MUSICBRAINZ_ARTISTID")
    tag.SetMusicBrainzArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ_ALBUMID")
    tag.SetMusicBrainzAlbumID(value);
  else if (key == "MUSICBRAINZ_RELEASEGROUPID")
    tag.SetMusicBrainzReleaseGroupID(value);
  else if (key == "MUSICBRAINZ_ALBUMARTISTID")
    tag.SetMusicBrainzAlbumArtistID(StringUtils::Split(value, separators));
  else if (key == "MUSICBRAINZ_TRACKID")
    tag.SetMusicBrainzTrackID(value);
  else if (key == "MUSICBRAINZ_ALBUMARTIST")
  {
    // tag.SetAlbumArtist(value);
  }
  else if (key == "MUSICBRAINZ_ALBUMTYPE")
    tag.SetMusicBrainzReleaseType(value);
  else if (key == "MUSICBRAINZ_ALBUMSTATUS")
    tag.SetAlbumReleaseStatus(value);
  else if (key == "ENCODED_BY" || key == "LANGUAGE")
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
    std::vector<std::string> tagdata = StringUtils::Split(value, ",");

    AddCommaDelimitedString(tagdata, separators, tag);
  }
  else if (key == "INSTRUMENTS")
  {
    std::vector<std::string> tagdata = StringUtils::Split(value, ",");

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
 * Static overload for external callers (e.g. AudioBookFileDirectory).
 * Opens its own KodiTagLibStream and delegates to the shared-stream overload.
*/
void CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(
    const std::string& fileName,
    std::map<std::string, std::string>& fileTags,
    std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
    std::vector<std::tuple<unsigned long long, std::string, double, double>>& chapterOrder,
    CMusicInfoTag* coverTag)
{
  KodiTagLibStream matroskaStream(fileName);
  if (!matroskaStream.open())
  {
    fileTags.clear();
    chapterTags.clear();
    chapterOrder.clear();
    return;
  }
  GetMatroskaMusicTags(fileName, matroskaStream, fileTags, chapterTags, chapterOrder, coverTag);
}

/*!
 * use TagLib to read hierarchy of tags in file and populate album and chapter
 * (track) tags. this creates a map of chapterUid to track tags for each chapter.
 * If coverTag is non-null, embedded cover art from Matroska attachments is set on it.
*/
void CMusicInfoTagLoaderMatroska::GetMatroskaMusicTags(
    const std::string& fileName,
    KodiTagLibStream& matroskaStream,
    std::map<std::string, std::string>& fileTags,
    std::map<unsigned long long, std::map<std::string, std::string>>& chapterTags,
    std::vector<std::tuple<unsigned long long, std::string, double, double>>& chapterOrder,
    CMusicInfoTag* coverTag,
    EmbeddedArt* art)
{
  fileTags.clear();
  chapterTags.clear();
  chapterOrder.clear();

  TagLib::Matroska::File* matroskaFile = nullptr;
  Matroska::Tag* matroskatag = nullptr;
  try
  {
    // KodiTagLibStream provides a 256 KiB read-ahead buffer and deferred
    // seeks, matching the BufferedIOStream used by the MMH interop project.
    // This eliminates per-read NFS/SMB round-trips when TagLib's EBML parser
    // makes thousands of tiny reads interspersed with large seeks.
    matroskaFile = new TagLib::Matroska::File(&matroskaStream, true, TagLib::AudioProperties::Fast);
    if (matroskaFile->isValid())
      matroskatag = matroskaFile->tag(true);
    if (!matroskatag)
    {
      delete matroskaFile;
      return;
    }

    // Get duration from TagLib AudioProperties if a coverTag is provided
    // (indicates this is called from Load(), not just for chapter enumeration)
    if (coverTag)
    {
      TagLib::AudioProperties* audioProps = matroskaFile->audioProperties();
      if (audioProps)
        coverTag->SetDuration(audioProps->lengthInSeconds());
    }

    // Read embedded cover art from attachments if requested
    if (coverTag)
      GetMatroskaEmbeddedCover(*matroskaFile, *coverTag, art);

    /*!
    * First get all chapters and get the chapter name for each chapter and store
    * it in the chapterTags map. Then we have chapter name for each chapter
    * (track) if Chapters are not tagged.
    */
    int chapterCount = 0;
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
            // FIX: Use first display only — previous code overwrote the map entry
            // for every display, so only the last survived
            std::string chapterName;
            if (!chapter.displayList().isEmpty())
              chapterName = chapter.displayList().front().string().toCString(true);

            std::map<std::string, std::string> chapterTagList = {{"CHAPTERNAME", chapterName}};
            chapterTags[chapter.uid()] = chapterTagList;

            double startTimeSecs = static_cast<double>(chapter.timeStart()) / 1000000000.0;
            double endTimeSecs = static_cast<double>(chapter.timeEnd()) / 1000000000.0;
            chapterOrder.push_back(
                std::make_tuple(chapter.uid(), chapterName, startTimeSecs, endTimeSecs));
            chapterCount++;
          }
        }
      }
    } // FIX: Close the if (chapters) block here — tag processing must happen
    // regardless of whether the file has a Chapters element

    /*!
    * For parsing Matroska tags create a dummy chapter if no chapters are present
    * to hold song tags for later processing for Kodi internal tags
    */
    unsigned long long DummyChapterUid = 999000999000999;
    if (chapterCount == 0)
    {
      chapterOrder.push_back(std::make_tuple(DummyChapterUid, std::string("SongTags"), 0.0, 0.0));
      std::map<std::string, std::string> chapterTagList = {{"CHAPTERNAME", "SongTags"}};
      chapterTags[DummyChapterUid] = chapterTagList;
    }

    /*!
    * Define tags that support multiple values and need to be concatenated into a
    * single internal Kodi tag with a semicolon separator if more than one value is
    * present. This is needed to support multiple values with Matroska
    */
    static constexpr std::array<const char*, 21> MULTIPLE_VALUE_TAGS = {
        "ALBUMARTISTS",
        "ALBUMARTISTSORT",
        "ARTIST",
        "ARTISTS",
        "ARTISTSORT",
        "ARRANGER",
        "BAND",
        "COMPOSER",
        "COMPOSERSORT",
        "CONDUCTOR",
        "ENGINEER",
        "GENRE",
        "LYRICIST",
        "MIXER",
        "MOOD",
        "MUSICBRAINZ_ALBUMARTISTID",
        "MUSICBRAINZ_ARTISTID",
        "PERFORMER",
        "PRODUCER",
        "REMIXED",
        "WRITER"}; // clang-format on

    /*!
    * Read all simple tags and group them by file (album or song files with no
    * chapters) or by chapter/track (if target type value is 30).
    * Delimiter separated lists are outside the Matroska spec
    * (see https://www.matroska.org/technical/tagging.html) it states to use
    * multiple simple tags for eg 2 or more composers. To ensure Kodi can use
    * multiple same name tags need create a single tag with multiple values in
    * a semicolon delimited string (Kodi handles multiple values with
    * delimited strings).
    *
    * Two pass approach:
    * Pass 1: Process album-level tags (targetTypeValue == 50) first so album
    *         metadata is established before track-level tags are processed.
    *         Special handling for TITLE tag which maps to ALBUM in Kodi.
    * Pass 2: Process file-level (targetTypeValue == 0) and chapter/song
    *         (targetTypeValue == 30) tags.
    */

    const TagLib::Matroska::SimpleTagsList& list = matroskatag->simpleTagsList();

    // Pass 1: Process album-level tags (targetTypeValue == 50)
    for (const TagLib::Matroska::SimpleTag& tag : list)
    {
      if (tag.targetTypeValue() == 50)
      {
        std::string TagName = StringUtils::ToUpper(tag.name().to8Bit(true));
        // TITLE with targetTypeValue 50 is the Album title in Matroska spec
        if (TagName == "TITLE")
        {
          if (fileTags.find("ALBUM") == fileTags.end())
            fileTags["ALBUM"] = tag.toString().to8Bit(true);
          if (fileTags.find("TITLE") == fileTags.end())
            fileTags["TITLE"] = tag.toString().to8Bit(true);
        }
        else if (fileTags.find(TagName) == fileTags.end())
        {
          fileTags[TagName] = tag.toString().to8Bit(true);
        }
        else
        {
          if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS), TagName) !=
              std::end(MULTIPLE_VALUE_TAGS))
          {
            std::string currentValue = fileTags[TagName];
            fileTags[TagName] = currentValue + " / " + tag.toString().to8Bit(true);
          }
        }
      }
    }

    // Pass 2: Process file-level (targetTypeValue == 0) and chapter/song (targetTypeValue == 30) tags
    for (const TagLib::Matroska::SimpleTag& tag : list)
    {
      unsigned long long chapterUid = tag.chapterUid();
      std::string TagName = StringUtils::ToUpper(tag.name().to8Bit(true));
      unsigned long long targetTypeValue = tag.targetTypeValue();

      if (targetTypeValue == 0)
      {
        // FIX: Removed redundant TagName re-declaration that shadowed outer variable
        // TITLE with targetTypeValue 0 maps to Album title (MKVToolNix default)
        if (TagName == "TITLE")
        {
          if (fileTags.find("ALBUM") == fileTags.end())
            fileTags["ALBUM"] = tag.toString().to8Bit(true);
          if (fileTags.find("TITLE") == fileTags.end())
            fileTags["TITLE"] = tag.toString().to8Bit(true);
        }
        else
        {
          if (fileTags.find(TagName) == fileTags.end())
          {
            fileTags[TagName] = tag.toString().to8Bit(true);
          }
          else
          {
            if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS),
                          TagName) != std::end(MULTIPLE_VALUE_TAGS))
            {
              std::string currentValue = fileTags[TagName];
              fileTags[TagName] = currentValue + " / " + tag.toString().to8Bit(true);
            }
          }
        }
      }
      else if (targetTypeValue == 30)
      {
        if (chapterCount == 1)
        {
          // Single chapter: route to the only chapter with duplicate check
          unsigned long long firstChapterUid = std::get<0>(chapterOrder[0]);
          auto firstIt = chapterTags.find(firstChapterUid);
          if (firstIt != chapterTags.end())
          {
            auto& chapterTagList = firstIt->second;
            auto it = chapterTagList.find(TagName);
            if (it == chapterTagList.end())
            {
              chapterTagList.emplace(TagName, tag.toString().to8Bit(true));
            }
            else
            {
              if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS),
                            TagName) != std::end(MULTIPLE_VALUE_TAGS))
              {
                std::string newValue = tag.toString().to8Bit(true);
                if (it->second.find(newValue) == std::string::npos)
                  it->second = it->second + " / " + newValue;
              }
            }
          }
        }
        else if (chapterUid > 0)
        {
          auto chapterIt = chapterTags.find(chapterUid);
          if (chapterIt != chapterTags.end())
          {
            auto& chapterTagList = chapterIt->second;
            auto it = chapterTagList.find(TagName);
            if (it == chapterTagList.end())
            {
              chapterTagList.emplace(TagName, tag.toString().to8Bit(true));
            }
            else
            {
              if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS),
                            TagName) != std::end(MULTIPLE_VALUE_TAGS))
              {
                std::string currentValue = it->second;
                it->second = currentValue + " / " + tag.toString().to8Bit(true);
              }
            }
          }
          else
          {
            // FIX: Corrected comment — chapterUid > 0 but not found in chapterTags,
            // so this chapter was not in the Chapters element. Fall back to fileTags.
            if (fileTags.find(TagName) == fileTags.end())
            {
              fileTags[TagName] = tag.toString().to8Bit(true);
            }
            else
            {
              if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS),
                            TagName) != std::end(MULTIPLE_VALUE_TAGS))
              {
                std::string currentValue = fileTags[TagName];
                fileTags[TagName] = currentValue + " / " + tag.toString().to8Bit(true);
              }
            }
          }
        }
        else
        {
          // FIX: Handle chapterUid == 0 with targetTypeValue == 30 and chapterCount > 1.
          // Some taggers (e.g. MP3Tag) save track tags without a chapterUid.
          // Fall back to fileTags so these are not silently dropped.
          if (fileTags.find(TagName) == fileTags.end())
          {
            fileTags[TagName] = tag.toString().to8Bit(true);
          }
          else
          {
            if (std::find(std::begin(MULTIPLE_VALUE_TAGS), std::end(MULTIPLE_VALUE_TAGS),
                          TagName) != std::end(MULTIPLE_VALUE_TAGS))
            {
              std::string currentValue = fileTags[TagName];
              fileTags[TagName] = currentValue + " / " + tag.toString().to8Bit(true);
            }
          }
        }
      }
    }

    // Cleanup — stream is owned by caller, only delete the TagLib file object.
    // bufferedStream is stack-allocated and will be destroyed when scope exits.
    delete matroskaFile;
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "GetMatroskaMusicTags: Exception while reading Matroska tags: {} {}",
              fileName, e.what());
    delete matroskaFile;
  }
}
