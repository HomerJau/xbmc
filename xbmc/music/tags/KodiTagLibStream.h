/*
 *  Copyright (C) 2005-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "filesystem/File.h"

#include <algorithm>
#include <string>
#include <taglib/tiostream.h>

/*!
 * \brief Thin VFS-backed TagLib IOStream adapter.
 *
 * Allows TagLib to read through Kodi's virtual filesystem
 * (supports nfs://, smb://, etc.)
 *
 * This class provides only a virtual-position optimisation: seeks are
 * deferred until the next readBlock() so that consecutive seek-then-seek
 * chains (common when TagLib skips Matroska Cluster elements) never
 * touch the network.
 *
 * All read-ahead buffering is delegated to TagLib's native
 * BufferedStream, which wraps this stream in
 * MusicInfoTagLoaderMatroska.cpp.  Keeping buffering in one place
 * (inside TagLib) avoids double-buffering and gives TagLib's EBML
 * parser optimal I/O coalescing.
 */
class KodiTagLibStream : public TagLib::IOStream
{
public:
  KodiTagLibStream(const std::string& fileName) : m_fileName(fileName) {}
  ~KodiTagLibStream() override { m_file.Close(); }

  TagLib::FileName name() const override { return m_fileName.c_str(); }

  bool isOpen() const override { return m_open; }

  bool open()
  {
    m_open = m_file.Open(m_fileName);
    if (m_open)
    {
      m_fileLength = m_file.GetLength();
      m_virtualPos = 0;
    }
    return m_open;
  }

  TagLib::ByteVector readBlock(size_t length) override
  {
    if (length == 0)
      return {};

    syncFilePosition(m_virtualPos);

    TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
    ssize_t bytesRead = m_file.Read(bv.data(), length);
    if (bytesRead > 0)
    {
      bv.resize(static_cast<unsigned int>(bytesRead));
      m_virtualPos += bytesRead;
      m_filePos = m_virtualPos;
    }
    else
      bv.clear();

    return bv;
  }

  void writeBlock(const TagLib::ByteVector&) override {}
  void insert(const TagLib::ByteVector&, TagLib::offset_t, size_t) override {}
  void removeBlock(TagLib::offset_t, size_t) override {}
  bool readOnly() const override { return true; }

  /*!
   * \brief Seek to a new position — updates only the virtual position.
   *
   * The actual CFile::Seek is deferred until the next readBlock().
   * This is critical for Matroska parsing where TagLib performs thousands
   * of seek cycles to skip past Cluster elements.  Many of those seeks
   * are followed by another seek before any read, so deferring avoids
   * thousands of NFS/SMB round-trips.
   */
  void seek(TagLib::offset_t offset, TagLib::IOStream::Position p) override
  {
    switch (p)
    {
      case TagLib::IOStream::Beginning:
        m_virtualPos = offset;
        break;
      case TagLib::IOStream::Current:
        m_virtualPos += offset;
        break;
      case TagLib::IOStream::End:
        m_virtualPos = m_fileLength + offset;
        break;
    }

    // Clamp to valid range
    if (m_virtualPos < 0)
      m_virtualPos = 0;
    if (m_virtualPos > m_fileLength)
      m_virtualPos = m_fileLength;
  }

  TagLib::offset_t tell() const override { return m_virtualPos; }

  TagLib::offset_t length() override { return m_fileLength; }

  void truncate(TagLib::offset_t) override {}
  void clear() override {}

  // Expose the underlying CFile for use by FFmpeg's AVIOContext
  XFILE::CFile& file() { return m_file; }

private:
  /*!
   * \brief Ensure the real CFile position matches the given position.
   *
   * Only issues a CFile::Seek if the real file position has diverged
   * from the requested position (i.e. after virtual-only seeks).
   */
  void syncFilePosition(int64_t pos)
  {
    if (m_filePos != pos)
    {
      m_file.Seek(pos, SEEK_SET);
      m_filePos = pos;
    }
  }

  std::string m_fileName;
  XFILE::CFile m_file;
  bool m_open = false;
  int64_t m_fileLength = 0;

  // Virtual file position — may diverge from the real CFile position
  // between seek() and the next readBlock(). This avoids costly VFS
  // seeks when TagLib seeks repeatedly without reading.
  int64_t m_virtualPos = 0;

  // Tracks the real CFile position so we can skip redundant Seek calls
  int64_t m_filePos = 0;
};
