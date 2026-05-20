/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DirectoryNodeAlbumTop100.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "music/Album.h"
#include "music/MusicDatabase.h"
#include "music/MusicDbUrl.h"
#include "music/tags/MusicInfoTag.h"
#include "utils/StringUtils.h"

using namespace XFILE::MUSICDATABASEDIRECTORY;

CDirectoryNodeAlbumTop100::CDirectoryNodeAlbumTop100(const std::string& strName,
                                                     CDirectoryNode* pParent)
  : CDirectoryNode(NodeType::ALBUM_TOP100, strName, pParent)
{
}

NodeType CDirectoryNodeAlbumTop100::GetChildType() const
{
  if (GetName() == "-1")
    return NodeType::ALBUM_TOP100_SONGS;

  return NodeType::SONG;
}

std::string CDirectoryNodeAlbumTop100::GetLocalizedName() const
{
  CMusicDatabase db;
  if (db.Open())
    return db.GetAlbumById(GetID());
  return "";
}

bool CDirectoryNodeAlbumTop100::GetContent(CFileItemList& items) const
{
  CMusicDatabase musicdatabase;
  if (!musicdatabase.Open())
    return false;

  std::vector<CAlbum> albums;
  if (!musicdatabase.GetTop100Albums(albums))
  {
    musicdatabase.Close();
    return false;
  }

  for (const CAlbum& album : albums)
  {
    // Build the musicdb:// URL with optional streamid option for the virtual
    // rendition. Mirrors GetAlbumsByWhere so the row reads like a distinct
    // rendition and playback honours the selection.
    CMusicDbUrl itemUrl;
    itemUrl.FromString(BuildPath());
    itemUrl.AppendPath(StringUtils::Format("{}/", album.idAlbum));
    if (album.idStreamDetail > 0)
      itemUrl.AddOption("streamid", album.iStream);

    CFileItemPtr pItem(new CFileItem(itemUrl.ToString(), album));
    if (album.idStreamDetail > 0)
    {
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
