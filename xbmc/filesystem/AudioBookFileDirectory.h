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

    protected:
      AVIOContext* m_ioctx = nullptr;
      AVFormatContext* m_fctx = nullptr;
      CFile m_file;

    private:
      static int GetSongCountFromDatabase(const CURL& url);

      /*!
       * \brief Ensure the FFmpeg format context (m_fctx) is open for codec info.
       *
       * If ContainsFiles() used the DB fast path, m_fctx will be null.
       * This opens it on demand so GetDirectory() can read codec parameters.
       *
       * \param url The file URL to open.
       * \return true if m_fctx is ready for use.
       */
      bool EnsureFFmpegContext(const CURL& url);
  };
}
