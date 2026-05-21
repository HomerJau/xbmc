/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "guilib/guiinfo/MusicGUIInfo.h"

#include "FileItem.h"
#include "PartyModeManager.h"
#include "PlayListPlayer.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "guilib/guiinfo/GUIInfo.h"
#include "guilib/guiinfo/GUIInfoHelper.h"
#include "guilib/guiinfo/GUIInfoLabels.h"
#include "guilib/guiinfo/GUIInfoUtils.h"
#include "music/MusicFileItemClassify.h"
#include "music/MusicInfoLoader.h"
#include "music/MusicThumbLoader.h"
#include "music/tags/MusicInfoTag.h"
#include "network/NetworkFileItemClassify.h"
#include "playlists/PlayList.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

using namespace KODI;
using namespace KODI::GUILIB;
using namespace KODI::GUILIB::GUIINFO;
using namespace MUSIC_INFO;

namespace
{
/*!
 * Map (codec, channel count, path) to a friendly channel-layout label
 * for skins. Three layers, checked in order:
 *
 *  1. Codec-based labels (Atmos, DTS:X) — unambiguous, win first.
 *  2. Path-based hints. Recover collection-specific intent that the
 *     raw channel count cannot express. The motivating case is 70s
 *     Quad music: many users store these files silent-channel-padded
 *     to 5.0 / 5.1 / 6.1 so legacy AVRs and older players recognise
 *     them as multichannel instead of down-mixing to stereo. Without
 *     a path hint, the channel-count switch below would label these
 *     "5.0" / "5.1" / "6.1" when they're really Quad.
 *  3. Standard channel-count → label mapping for everything else.
 *
 * Returns an empty string when no rule applies — skins can fall back
 * to ListItem.MusicCodec or ListItem.MusicChannels.
 *
 * The `path` argument is the file's full path (caller supplies
 * tag->GetURL() with item->GetPath() fallback). Hint rules match
 * against the entire path so parent folder names ("Quad/", "5.1/",
 * "Quadio/") feed into the decision.
 */
std::string MakeMusicChannelsString(const std::string& codec,
                                    int channels,
                                    const std::string& path)
{
  // --- Layer 1: codec-based labels ----------------------------------------
  if (codec == "eac3_ddp_atmos" || codec == "truehd_atmos")
    return "Atmos";
  if (codec == "dtshd_ma_x")
    return "DTS:X";

  // --- Layer 2: path-based hints ------------------------------------------
  //
  // Order matters — more-specific patterns must come BEFORE general ones,
  // because the first matching rule wins. Each rule is one if-block of
  // the form:
  //
  //     if (<predicate on path / channels>)
  //       return "<label>";
  //
  // For substring matches use std::string::find:
  //     path.find("Quad/") != std::string::npos
  //
  // For multi-condition rules combine with && and the channel count:
  //     if (channels == 6 && path.find("Penteo") != std::string::npos
  //                       && path.find("Quad") != std::string::npos)
  //       return "4.1 UM";
  //
  // Case sensitivity: std::string::find is case-SENSITIVE. If folder names
  // in the collection vary in case, use StringUtils::FindWords (matches
  // whole words, case-insensitive) or lowercase a copy of path once at the
  // top of this block.
  //
  // ===== HINT RULES =======================================================
  //
  // Quad-padded-with-silence detection. Many 70s Quad files are stored
  // silent-padded to 5ch / 6ch so legacy AVRs and older players recognise
  // them as multichannel instead of down-mixing to stereo; the real intent
  // is "Quad", not "5.0" / "5.1". Without these rules the channel-count
  // switch below would mis-label them.
  //
  // Gate on channels in {4,5,6} so true 5.1 movie-style multichannel and
  // 7.1 files skip these rules entirely.
  if (channels >= 4 && channels <= 6)
  {
    // Most-specific first: Penteo upmix of a Quad source. Garry's
    // collection labels these "4.1 UM" when 6ch (Quad + LFE + silent) and
    // "Quad UM" when 5ch (Quad + silent rear).
    //
    // Use "Quad/" with the trailing slash so this matches only the literal
    // parent folder "Quad/" and NOT album names that happen to start with
    // "Quad" (e.g. The Who's "Quadrophenia", "Quadromania", "Quadrille"
    // etc. would false-positive otherwise).
    if (path.find("Penteo") != std::string::npos &&
        path.find("Quad/")  != std::string::npos)
    {
      return channels == 6 ? "4.1 UM" : "Quad UM";
    }

    // "Quadio" branding — a specific Quad label/series; show its name.
    if (path.find("Quadio") != std::string::npos)
      return "Quadio";

    // Generic Quad folder hint (e.g. .../Quad/<artist>/<album>/...) or
    // a "4.0" tag anywhere in the path.
    if (path.find("Quad/") != std::string::npos ||
        path.find("4.0")   != std::string::npos)
      return "Quad";
  }
  // ========================================================================

  // --- Layer 3: standard channel-count → label ----------------------------
  switch (channels)
  {
    case 1: return "Mono";
    case 2: return "Stereo";
    case 3: return "3.0";
    case 4: return "Quad";
    case 5: return "5.0";
    case 6: return "5.1";
    case 7: return "6.1";
    case 8: return "7.1";
    default: break;
  }
  if (channels >= 9)
    return StringUtils::Format("{}.0", channels);
  return {};
}
} // namespace

bool CMusicGUIInfo::InitCurrentItem(CFileItem* item)
{
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (item &&
      (MUSIC::IsAudio(*item) || (NETWORK::IsInternetStream(*item) && appPlayer->IsPlayingAudio())))
  {
    CLog::Log(LOGDEBUG, "CMusicGUIInfo::InitCurrentItem({})", item->GetPath());

    item->LoadMusicTag();

    CMusicInfoTag* tag =
        item->GetMusicInfoTag(); // creates item if not yet set, so no nullptr checks needed
    tag->SetLoaded(true);

    // find a thumb for this file.
    if (NETWORK::IsInternetStream(*item) && !MUSIC::IsMusicDb(*item))
    {
      if (!g_application.m_strPlayListFile.empty())
      {
        CLog::Log(LOGDEBUG, "Streaming media detected... using {} to find a thumb",
                  g_application.m_strPlayListFile);
        CFileItem streamingItem(g_application.m_strPlayListFile, false);

        CMusicThumbLoader loader;
        loader.FillThumb(streamingItem);
        if (streamingItem.HasArt("thumb"))
          item->SetArt("thumb", streamingItem.GetArt("thumb"));
      }
    }
    else
    {
      CMusicThumbLoader loader;
      loader.LoadItem(item);
    }

    CMusicInfoLoader::LoadAdditionalTagInfo(item);
    return true;
  }
  return false;
}

bool CMusicGUIInfo::GetLabel(std::string& value,
                             const CFileItem* item,
                             int contextWindow,
                             const CGUIInfo& info,
                             std::string* fallback) const
{
  // For musicplayer "offset" and "position" info labels check playlist
  if (info.GetData1() && ((info.GetInfo() >= MUSICPLAYER_OFFSET_POSITION_FIRST &&
                           info.GetInfo() <= MUSICPLAYER_OFFSET_POSITION_LAST) ||
                          (info.GetInfo() >= PLAYER_OFFSET_POSITION_FIRST &&
                           info.GetInfo() <= PLAYER_OFFSET_POSITION_LAST)))
    return GetPlaylistInfo(value, info);

  const CMusicInfoTag* tag = item->GetMusicInfoTag();
  if (tag)
  {
    switch (info.GetInfo())
    {
      /////////////////////////////////////////////////////////////////////////////////////////////
      // PLAYER_* / MUSICPLAYER_* / LISTITEM_*
      /////////////////////////////////////////////////////////////////////////////////////////////
      case PLAYER_PATH:
      case PLAYER_FILENAME:
      case PLAYER_FILEPATH:
        value = tag->GetURL();
        if (value.empty())
          value = item->GetPath();
        value = GUIINFO::GetFileInfoLabelValueFromPath(info.GetInfo(), value);
        return true;
      case PLAYER_TITLE:
      case MUSICPLAYER_TITLE:
        value = tag->GetTitle();
        return !value.empty();
      case LISTITEM_TITLE:
        value = tag->GetTitle();
        return true;
      case MUSICPLAYER_PLAYCOUNT:
      case LISTITEM_PLAYCOUNT:
        if (tag->GetPlayCount() > 0)
        {
          value = std::to_string(tag->GetPlayCount());
          return true;
        }
        break;
      case MUSICPLAYER_LASTPLAYED:
      case LISTITEM_LASTPLAYED:
      {
        const CDateTime& dateTime = tag->GetLastPlayed();
        if (dateTime.IsValid())
        {
          value = dateTime.GetAsLocalizedDate();
          return true;
        }
        break;
      }
      case MUSICPLAYER_TRACK_NUMBER:
      case LISTITEM_TRACKNUMBER:
        if (tag->Loaded() && tag->GetTrackNumber() > 0)
        {
          value = StringUtils::Format("{:02}", tag->GetTrackNumber());
          return true;
        }
        break;
      case MUSICPLAYER_DISC_NUMBER:
      case LISTITEM_DISC_NUMBER:
        if (tag->GetDiscNumber() > 0)
        {
          value = std::to_string(tag->GetDiscNumber());
          return true;
        }
        break;
      case MUSICPLAYER_TOTALDISCS:
      case LISTITEM_TOTALDISCS:
        value = std::to_string(tag->GetTotalDiscs());
        return true;
      case MUSICPLAYER_DISC_TITLE:
      case LISTITEM_DISC_TITLE:
        value = tag->GetDiscSubtitle();
        return true;
      case MUSICPLAYER_ARTIST:
      case LISTITEM_ARTIST:
        value = tag->GetArtistString();
        return true;
      case MUSICPLAYER_ALBUM_ARTIST:
      case LISTITEM_ALBUM_ARTIST:
        value = tag->GetAlbumArtistString();
        return true;
      case MUSICPLAYER_CONTRIBUTORS:
      case LISTITEM_CONTRIBUTORS:
        if (tag->HasContributors())
        {
          value = tag->GetContributorsText();
          return true;
        }
        break;
      case MUSICPLAYER_CONTRIBUTOR_AND_ROLE:
      case LISTITEM_CONTRIBUTOR_AND_ROLE:
        if (tag->HasContributors())
        {
          value = tag->GetContributorsAndRolesText();
          return true;
        }
        break;
      case MUSICPLAYER_ALBUM:
      case LISTITEM_ALBUM:
        value = tag->GetAlbum();
        return true;
      case MUSICPLAYER_YEAR:
      case LISTITEM_YEAR:
        value = tag->GetYearString();
        return true;
      case MUSICPLAYER_GENRE:
      case LISTITEM_GENRE:
      {
        const std::string sep{info.GetData3().empty() ? CServiceBroker::GetSettingsComponent()
                                                            ->GetAdvancedSettings()
                                                            ->m_musicItemSeparator
                                                      : info.GetData3()};
        value = StringUtils::Join(tag->GetGenre(), sep);
        return true;
      }
      case MUSICPLAYER_LYRICS:
        value = tag->GetLyrics();
        return true;
      case MUSICPLAYER_RATING:
      case LISTITEM_RATING:
      {
        float rating = tag->GetRating();
        if (rating > 0.f)
        {
          value = StringUtils::FormatNumber(rating);
          return true;
        }
        break;
      }
      case MUSICPLAYER_RATING_AND_VOTES:
      case LISTITEM_RATING_AND_VOTES:
      {
        float rating = tag->GetRating();
        if (rating > 0.f)
        {
          int votes = tag->GetVotes();
          if (votes <= 0)
            value = StringUtils::FormatNumber(rating);
          else
            value = StringUtils::Format(
                CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(20350),
                StringUtils::FormatNumber(rating), StringUtils::FormatNumber(votes));
          return true;
        }
        break;
      }
      case MUSICPLAYER_USER_RATING:
      case LISTITEM_USER_RATING:
        if (tag->GetUserrating() > 0)
        {
          value = std::to_string(tag->GetUserrating());
          return true;
        }
        break;
      case MUSICPLAYER_COMMENT:
      case LISTITEM_COMMENT:
        value = tag->GetComment();
        return true;
      case MUSICPLAYER_MOOD:
      case LISTITEM_MOOD:
        value = tag->GetMood();
        return true;
      case LISTITEM_DBTYPE:
        value = tag->GetType();
        return true;
      case MUSICPLAYER_DBID:
      case LISTITEM_DBID:
      {
        int dbId = tag->GetDatabaseId();
        if (dbId > -1)
        {
          value = std::to_string(dbId);
          return true;
        }
        break;
      }
      case PLAYER_DURATION:
      {
        const auto& components = CServiceBroker::GetAppComponents();
        const auto appPlayer = components.GetComponent<CApplicationPlayer>();
        if (!appPlayer->IsPlayingAudio())
          break;
      }
        [[fallthrough]];
      case MUSICPLAYER_DURATION:
      case LISTITEM_DURATION:
      {
        int iDuration = tag->GetDuration();
        if (iDuration > 0)
        {
          value = StringUtils::SecondsToTimeString(
              iDuration,
              static_cast<TIME_FORMAT>(info.GetInfo() == LISTITEM_DURATION ? info.GetData4()
                                                                           : info.GetData1()));
          return true;
        }
        break;
      }
      case MUSICPLAYER_BPM:
      case LISTITEM_BPM:
        if (tag->GetBPM() > 0)
        {
          value = std::to_string(tag->GetBPM());
          return true;
        }
        break;
      case MUSICPLAYER_STATIONNAME:
        // This property can be used for example by addons to enforce/override the station name.
        value = item->GetProperty("StationName").asString();
        if (value.empty())
          value = tag->GetStationName();
        return true;

      /////////////////////////////////////////////////////////////////////////////////////////////
      // LISTITEM_*
      /////////////////////////////////////////////////////////////////////////////////////////////
      case LISTITEM_PROPERTY:
        if (StringUtils::StartsWithNoCase(info.GetData3(), "Role."))
        {
          // "Role.xxxx" properties are held in music tag
          std::string property = info.GetData3();
          property.erase(0, 5); //Remove Role.
          value = tag->GetArtistStringForRole(property);
          return true;
        }
        break;
      case LISTITEM_VOTES:
        value = StringUtils::FormatNumber(tag->GetVotes());
        return true;
      case MUSICPLAYER_ORIGINALDATE:
      case LISTITEM_ORIGINALDATE:
      {
        value = tag->GetOriginalDate();
        if (!CServiceBroker::GetSettingsComponent()
                 ->GetAdvancedSettings()
                 ->m_bMusicLibraryUseISODates)
          value = StringUtils::ISODateToLocalizedDate(value);
        return true;
      }
      case MUSICPLAYER_RELEASEDATE:
      case LISTITEM_RELEASEDATE:
      {
        value = tag->GetReleaseDate();
        if (!CServiceBroker::GetSettingsComponent()
                 ->GetAdvancedSettings()
                 ->m_bMusicLibraryUseISODates)
          value = StringUtils::ISODateToLocalizedDate(value);
        return true;
      }
      break;
      case LISTITEM_BITRATE:
      {
        int BitRate = tag->GetBitRate();
        if (BitRate > 0)
        {
          if (BitRate > 100000)
            value = StringUtils::Format("{:.0f}", static_cast<double>(BitRate) / 1000.0);
          else
            value = std::to_string(BitRate);
          return true;
        }
        break;
      }
      case LISTITEM_SAMPLERATE:
      {
        int sampleRate = tag->GetSampleRate();
        if (sampleRate > 0)
        {
          value = StringUtils::Format("{:.5}", static_cast<double>(sampleRate) / 1000.0);
          return true;
        }
        break;
      }
      case LISTITEM_MUSICCHANNELS:
      {
        const auto formatted{
            CGUIInfoUtils::FormatAudioChannels(info.GetData3(), tag->GetNoOfChannels())};

        if (formatted.has_value())
        {
          value = formatted.value();
          return true;
        }
        break;
      }
      case LISTITEM_MUSIC_BITSPERSAMPLE:
      {
        int bitsPerSample = tag->GetBitsPerSample();
        if (bitsPerSample > 0)
        {
          value = std::to_string(bitsPerSample);
          return true;
        }
        break;
      }
      case LISTITEM_MUSIC_CODEC:
      case MUSICPLAYER_CODEC:
        value = tag->GetCodec();
        return true;

      case LISTITEM_MUSIC_CHANNELS_STRING:
      {
        // Pass the file path so MakeMusicChannelsString can apply
        // collection-specific path-hint rules (e.g. "padded Quad" recovery).
        // tag->GetURL() is the canonical music-file URL set by the DB
        // load path; fall back to item->GetPath() for items that lack
        // a tag-side URL (rare; defensive).
        std::string path = tag->GetURL();
        if (path.empty())
          path = item->GetPath();
        value = MakeMusicChannelsString(tag->GetCodec(), tag->GetNoOfChannels(), path);
        return true;
      }

      case LISTITEM_MUSIC_VIDEO_CODEC:
      case MUSICPLAYER_VIDEO_CODEC:
        if (tag->HasVideoStream())
          value = tag->GetVideoStream().strVideoCodec;
        return true;

      case LISTITEM_MUSIC_VIDEO_WIDTH:
      case MUSICPLAYER_VIDEO_WIDTH:
        if (tag->HasVideoStream() && tag->GetVideoStream().iVideoWidth > 0)
        {
          value = std::to_string(tag->GetVideoStream().iVideoWidth);
          return true;
        }
        break;

      case LISTITEM_MUSIC_VIDEO_HEIGHT:
      case MUSICPLAYER_VIDEO_HEIGHT:
        if (tag->HasVideoStream() && tag->GetVideoStream().iVideoHeight > 0)
        {
          value = std::to_string(tag->GetVideoStream().iVideoHeight);
          return true;
        }
        break;

      case LISTITEM_MUSIC_VIDEO_RESOLUTION:
      case MUSICPLAYER_VIDEO_RESOLUTION:
        if (tag->HasVideoStream())
        {
          const int w = tag->GetVideoStream().iVideoWidth;
          const int h = tag->GetVideoStream().iVideoHeight;
          if (w > 0 && h > 0)
          {
            value = StringUtils::Format("{}x{}", w, h);
            return true;
          }
        }
        break;

      case LISTITEM_MUSIC_HDR_TYPE:
      case MUSICPLAYER_HDR_TYPE:
        if (tag->HasVideoStream())
          value = tag->GetVideoStream().strHdrType;
        return true;

      case LISTITEM_ALBUMSTATUS:
        value = tag->GetAlbumReleaseStatus();
        return true;
      case LISTITEM_FILENAME:
      case LISTITEM_FILE_EXTENSION:
      case LISTITEM_FILENAME_NO_EXTENSION:
        if (MUSIC::IsMusicDb(*item))
          value = URIUtils::GetFileName(tag->GetURL());
        else if (
            item->HasVideoInfoTag()) // special handling for music videos, which have both a videotag and a musictag
          break;
        else
          value = URIUtils::GetFileName(item->GetPath());

        if (info.GetInfo() == LISTITEM_FILE_EXTENSION)
        {
          std::string strExtension = URIUtils::GetExtension(value);
          value = StringUtils::TrimLeft(strExtension, ".");
        }
        else if (info.GetInfo() == LISTITEM_FILENAME_NO_EXTENSION)
        {
          URIUtils::RemoveExtension(value);
        }
        return true;
      case LISTITEM_FOLDERNAME:
      case LISTITEM_PATH:
        if (MUSIC::IsMusicDb(*item))
          value = URIUtils::GetDirectory(tag->GetURL());
        else if (
            item->HasVideoInfoTag()) // special handling for music videos, which have both a videotag and a musictag
          break;
        else
          URIUtils::GetParentPath(item->GetPath(), value);

        value = CURL(value).GetWithoutUserDetails();

        if (info.GetInfo() == LISTITEM_FOLDERNAME)
        {
          URIUtils::RemoveSlashAtEnd(value);
          value = URIUtils::GetFileName(value);
        }
        return true;
      case LISTITEM_FILENAME_AND_PATH:
        if (MUSIC::IsMusicDb(*item))
          value = tag->GetURL();
        else if (
            item->HasVideoInfoTag()) // special handling for music videos, which have both a videotag and a musictag
          break;
        else
          value = item->GetPath();

        value = CURL(value).GetWithoutUserDetails();
        return true;
      case LISTITEM_DATE_ADDED:
        if (tag->GetDateAdded().IsValid())
        {
          value = tag->GetDateAdded().GetAsLocalizedDate();
          return true;
        }
        break;
      case LISTITEM_SONG_VIDEO_URL:
        value = tag->GetSongVideoURL();
        return true;
      default:
        break;
    }
  }

  switch (info.GetInfo())
  {
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // MUSICPLAYER_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case MUSICPLAYER_PROPERTY:
      if (StringUtils::StartsWithNoCase(info.GetData3(), "Role.") && item->HasMusicInfoTag())
      {
        // "Role.xxxx" properties are held in music tag
        std::string property = info.GetData3();
        property.erase(0, 5); //Remove Role.
        value = item->GetMusicInfoTag()->GetArtistStringForRole(property);
        return true;
      }
      value = item->GetProperty(info.GetData3()).asString();
      return true;
    case MUSICPLAYER_PLAYLISTLEN:
      if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST::Id::TYPE_MUSIC)
      {
        value = GUIINFO::GetPlaylistLabel(PLAYLIST_LENGTH);
        return true;
      }
      break;
    case MUSICPLAYER_PLAYLISTPOS:
      if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST::Id::TYPE_MUSIC)
      {
        value = GUIINFO::GetPlaylistLabel(PLAYLIST_POSITION);
        return true;
      }
      break;
    case MUSICPLAYER_COVER:
    {
      const auto& components = CServiceBroker::GetAppComponents();
      const auto appPlayer = components.GetComponent<CApplicationPlayer>();
      if (appPlayer->IsPlayingAudio())
      {
        if (fallback)
          *fallback = "DefaultAlbumCover.png";
        value = item->HasArt("thumb") ? item->GetArt("thumb") : "DefaultAlbumCover.png";
        return true;
      }
      break;
    }
    case MUSICPLAYER_BITRATE:
    {
      int iBitrate = m_audioInfo.bitrate;
      if (iBitrate > 0)
      {
        value = std::to_string(std::lrint(static_cast<double>(iBitrate) / 1000.0));
        return true;
      }
      break;
    }
    case MUSICPLAYER_CHANNELS:
    {
      const auto formatted{
          CGUIInfoUtils::FormatAudioChannels(info.GetData3(), m_audioInfo.channels)};

      if (formatted.has_value())
      {
        value = formatted.value();
        return true;
      }
      break;
    }
    case MUSICPLAYER_BITSPERSAMPLE:
    {
      int iBPS = m_audioInfo.bitspersample;
      if (iBPS > 0)
      {
        value = std::to_string(iBPS);
        return true;
      }
      break;
    }
    case MUSICPLAYER_SAMPLERATE:
    {
      int iSamplerate = m_audioInfo.samplerate;
      if (iSamplerate > 0)
      {
        value = StringUtils::Format("{:.5}", static_cast<double>(iSamplerate) / 1000.0);
        return true;
      }
      break;
    }
    default:
      break;
  }

  ///////////////////////////////////////////////////////////////////////////////////////////////
  // MUSICPM_*
  ///////////////////////////////////////////////////////////////////////////////////////////////
  if (GetPartyModeLabel(value, info))
    return true;

  return false;
}

bool CMusicGUIInfo::GetPartyModeLabel(std::string& value, const CGUIInfo& info) const
{
  int iSongs = -1;

  switch (info.GetInfo())
  {
    case MUSICPM_SONGSPLAYED:
      iSongs = g_partyModeManager.GetSongsPlayed();
      break;
    case MUSICPM_MATCHINGSONGS:
      iSongs = g_partyModeManager.GetMatchingSongs();
      break;
    case MUSICPM_MATCHINGSONGSPICKED:
      iSongs = g_partyModeManager.GetMatchingSongsPicked();
      break;
    case MUSICPM_MATCHINGSONGSLEFT:
      iSongs = g_partyModeManager.GetMatchingSongsLeft();
      break;
    case MUSICPM_RELAXEDSONGSPICKED:
      iSongs = g_partyModeManager.GetRelaxedSongs();
      break;
    case MUSICPM_RANDOMSONGSPICKED:
      iSongs = g_partyModeManager.GetRandomSongs();
      break;
    default:
      break;
  }

  if (iSongs >= 0)
  {
    value = std::to_string(iSongs);
    return true;
  }

  return false;
}

bool CMusicGUIInfo::GetPlaylistInfo(std::string& value, const CGUIInfo& info) const
{
  const PLAYLIST::CPlayList& playlist =
      CServiceBroker::GetPlaylistPlayer().GetPlaylist(PLAYLIST::Id::TYPE_MUSIC);
  if (playlist.size() < 1)
    return false;

  int index = info.GetData2();
  if (info.GetData1() == 1)
  { // relative index (requires current playlist is TYPE_MUSIC)
    if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() != PLAYLIST::Id::TYPE_MUSIC)
      return false;

    index = CServiceBroker::GetPlaylistPlayer().GetNextItemIdx(index);
  }

  if (index < 0 || index >= playlist.size())
    return false;

  const CFileItemPtr playlistItem = playlist[index];
  if (playlistItem->HasMusicInfoTag() && !playlistItem->GetMusicInfoTag()->Loaded())
  {
    playlistItem->LoadMusicTag();
    playlistItem->GetMusicInfoTag()->SetLoaded();
  }
  // try to set a thumbnail
  if (!playlistItem->HasArt("thumb"))
  {
    CMusicThumbLoader loader;
    loader.LoadItem(playlistItem.get());
    // still no thumb? then just the set the default cover
    if (!playlistItem->HasArt("thumb"))
      playlistItem->SetArt("thumb", "DefaultAlbumCover.png");
  }
  if (info.GetInfo() == MUSICPLAYER_PLAYLISTPOS)
  {
    value = std::to_string(index + 1);
    return true;
  }
  else if (info.GetInfo() == MUSICPLAYER_COVER)
  {
    value = playlistItem->GetArt("thumb");
    return true;
  }

  return GetLabel(value, playlistItem.get(), 0, CGUIInfo(info.GetInfo()), nullptr);
}

bool CMusicGUIInfo::GetFallbackLabel(std::string& value,
                                     const CFileItem* item,
                                     int contextWindow,
                                     const CGUIInfo& info,
                                     std::string* fallback)
{
  // No fallback for musicplayer "offset" and "position" info labels
  if (info.GetData1() && ((info.GetInfo() >= MUSICPLAYER_OFFSET_POSITION_FIRST &&
                           info.GetInfo() <= MUSICPLAYER_OFFSET_POSITION_LAST) ||
                          (info.GetInfo() >= PLAYER_OFFSET_POSITION_FIRST &&
                           info.GetInfo() <= PLAYER_OFFSET_POSITION_LAST)))
    return false;

  const CMusicInfoTag* tag = item->GetMusicInfoTag();
  if (tag)
  {
    switch (info.GetInfo())
    {
      /////////////////////////////////////////////////////////////////////////////////////////////
      // MUSICPLAYER_*
      /////////////////////////////////////////////////////////////////////////////////////////////
      case MUSICPLAYER_TITLE:
        value = item->GetLabel();
        if (value.empty())
          value = CUtil::GetTitleFromPath(item->GetPath());
        return true;
      default:
        break;
    }
  }
  return false;
}

bool CMusicGUIInfo::GetInt(int& value,
                           const CGUIListItem* gitem,
                           int contextWindow,
                           const CGUIInfo& info) const
{
  return false;
}

bool CMusicGUIInfo::GetBool(bool& value,
                            const CGUIListItem* gitem,
                            int contextWindow,
                            const CGUIInfo& info) const
{
  const auto* item{static_cast<const CFileItem*>(gitem)};
  const CMusicInfoTag* tag = item->GetMusicInfoTag();

  switch (info.GetInfo())
  {
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // MUSICPLAYER_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case MUSICPLAYER_CONTENT:
      value = StringUtils::EqualsNoCase(info.GetData3(), "files");
      return value; // if no match for this provider, other providers shall be asked.
    case MUSICPLAYER_HASPREVIOUS:
      // requires current playlist be TYPE_MUSIC
      if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST::Id::TYPE_MUSIC)
      {
        value = (CServiceBroker::GetPlaylistPlayer().GetCurrentItemIdx() > 0); // not first song
        return true;
      }
      break;
    case MUSICPLAYER_HASNEXT:
      // requires current playlist be TYPE_MUSIC
      if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST::Id::TYPE_MUSIC)
      {
        value = (CServiceBroker::GetPlaylistPlayer().GetCurrentItemIdx() <
                 (CServiceBroker::GetPlaylistPlayer().GetPlaylist(PLAYLIST::Id::TYPE_MUSIC).size() -
                  1)); // not last song
        return true;
      }
      break;
    case MUSICPLAYER_PLAYLISTPLAYING:
    {
      const auto& components = CServiceBroker::GetAppComponents();
      const auto appPlayer = components.GetComponent<CApplicationPlayer>();
      if (appPlayer->IsPlayingAudio() &&
          CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() == PLAYLIST::Id::TYPE_MUSIC)
      {
        value = true;
        return true;
      }
      break;
    }
    case MUSICPLAYER_EXISTS:
    {
      int index = info.GetData2();
      if (info.GetData1() == 1)
      { // relative index
        if (CServiceBroker::GetPlaylistPlayer().GetCurrentPlaylist() != PLAYLIST::Id::TYPE_MUSIC)
        {
          value = false;
          return true;
        }
        index += CServiceBroker::GetPlaylistPlayer().GetCurrentItemIdx();
      }
      value =
          (index >= 0 &&
           index <
               CServiceBroker::GetPlaylistPlayer().GetPlaylist(PLAYLIST::Id::TYPE_MUSIC).size());
      return true;
    }
    case MUSICPLAYER_ISMULTIDISC:
      if (tag)
      {
        value = (item->GetMusicInfoTag()->GetTotalDiscs() > 1);
        return true;
      }
      break;
    case LISTITEM_MUSIC_HAS_VIDEO_STREAM:
    case MUSICPLAYER_HAS_VIDEO_STREAM:
      if (tag)
      {
        value = tag->HasVideoStream();
        return true;
      }
      break;
    ///////////////////////////////////////////////////////////////////////////////////////////////
    // MUSICPM_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case MUSICPM_ENABLED:
      value = g_partyModeManager.IsEnabled();
      return true;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // LISTITEM_*
    ///////////////////////////////////////////////////////////////////////////////////////////////
    case LISTITEM_IS_BOXSET:
      if (tag)
      {
        value = tag->GetBoxset() == true;
        return true;
      }
      break;
    default:
      break;
  }

  return false;
}
