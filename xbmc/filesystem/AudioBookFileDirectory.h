/*
 *  Copyright (C) 2014 Arne Morten Kvarving
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "File.h"
#include "IFileDirectory.h"
#include "music/tags/MusicInfoTag.h"
#include <memory>
extern "C" {
#include <libavformat/avformat.h>
}

namespace XFILE
{
  class CAudioBookFileDirectory : public IFileDirectory
  {
    public:
      ~CAudioBookFileDirectory(void) override;
      bool GetDirectory(const CURL& url, CFileItemList &items) override;
      bool Exists(const CURL& url) override;
      bool ContainsFiles(const CURL& url) override;
      bool IsAllowed(const CURL& url) const override { return true; }
      /*!
       * Check if a file already has multiple chapter/song records in the music DB.
       * Used by CFileDirectoryFactory to short-circuit the expensive file parse
       * during playback. If the file is already scanned, there is no need to
       * create an AudioBookFileDirectory or open the file at all.
       *
       * \param url The file URL to check.
       * \return true if the DB contains > 1 song for this file (i.e. already scanned).
       */
      static bool HasChaptersInDatabase(const CURL& url);

    protected:
      AVIOContext* m_ioctx = nullptr;
      AVFormatContext* m_fctx = nullptr;
      CFile m_file;

    private:
      static int GetSongCountFromDatabase(const CURL& url);
  };
}
