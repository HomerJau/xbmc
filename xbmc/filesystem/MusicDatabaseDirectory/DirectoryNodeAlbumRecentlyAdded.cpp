/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DirectoryNodeAlbumRecentlyAdded.h"
#include "FileItem.h"
#include "FileItemList.h"
#include "ServiceBroker.h"
#include "music/Album.h"
#include "music/MusicDatabase.h"
#include "music/MusicDbUrl.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "music/tags/MusicInfoTag.h"
#include "utils/StringUtils.h"

using namespace XFILE::MUSICDATABASEDIRECTORY;
using namespace MUSIC_INFO;

CDirectoryNodeAlbumRecentlyAdded::CDirectoryNodeAlbumRecentlyAdded(const std::string& strName,
                                                                   CDirectoryNode* pParent)
  : CDirectoryNode(NodeType::ALBUM_RECENTLY_ADDED, strName, pParent)
{

}

NodeType CDirectoryNodeAlbumRecentlyAdded::GetChildType() const
{
  if (GetName() == "-1")
    return NodeType::ALBUM_RECENTLY_ADDED_SONGS;

  return NodeType::DISC;
}

std::string CDirectoryNodeAlbumRecentlyAdded::GetLocalizedName() const
{
  if (GetID() == -1)
    return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(15102); // All Albums
  CMusicDatabase db;
  if (db.Open())
    return db.GetAlbumById(GetID());
  return "";
}

bool CDirectoryNodeAlbumRecentlyAdded::GetContent(CFileItemList& items) const
{
  CMusicDatabase musicdatabase;
  if (!musicdatabase.Open())
    return false;

  std::vector<CAlbum> albums;
  if (!musicdatabase.GetRecentlyAddedAlbums(albums))
  {
    musicdatabase.Close();
    return false;
  }

  for (const CAlbum& album : albums)
  {
    // Build the musicdb:// URL with optional streamid option for the virtual
    // rendition. Mirrors what GetAlbumsByWhere does for the main Albums
    // browser so a click here lands on the same multi-stream-aware path.
    CMusicDbUrl itemUrl;
    itemUrl.FromString(BuildPath());
    itemUrl.AppendPath(StringUtils::Format("{}/", album.idAlbum));
    if (album.idStreamDetail > 0)
      itemUrl.AddOption("streamid", album.iStream);

    std::string albumPath = musicdatabase.GetPathForAlbum(album.idAlbum);
    CFileItemPtr pItem(new CFileItem(itemUrl.ToString(), album));
    pItem->GetMusicInfoTag()->SetURL(albumPath);
    if (album.idStreamDetail > 0)
    {
      // Per-rendition label suffix + preferred-stream hint so the row reads
      // as a distinct rendition and playback honours the selection.
      if (!album.strCodec.empty())
      {
        std::string suffix = " [" + album.strCodec;
        if (album.iChannels > 0)
          suffix += StringUtils::Format(" {}ch", album.iChannels);
        suffix += "]";
        pItem->SetLabel(pItem->GetLabel() + suffix);
      }
      pItem->GetMusicInfoTag()->SetPreferredAudioStreamIndex(album.iStream);
    }
    items.Add(pItem);
  }

  musicdatabase.Close();
  return true;
}
