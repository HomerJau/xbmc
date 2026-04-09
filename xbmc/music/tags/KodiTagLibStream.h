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
#include <vector>
#include <taglib/tiostream.h>

/*!
 * \brief VFS-backed TagLib IOStream adapter with read-ahead buffering.
 *
 * Allows TagLib to read through Kodi's virtual filesystem
 * (supports nfs://, smb://, etc.)
 *
 * TagLib performs many small reads (often just a few bytes) interspersed
 * with seeks.  Over network VFS backends each of those tiny reads becomes
 * a round-trip, which can stall Kodi noticeably.  This class keeps an
 * internal read-ahead buffer so that sequential small reads are served
 * from memory and the underlying CFile is only touched when the request
 * falls outside the buffered window.
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
      m_fileLength = m_file.GetLength();
    return m_open;
  }

  TagLib::ByteVector readBlock(size_t length) override
  {
    if (length == 0)
      return {};

    const int64_t pos = m_file.GetPosition();

    // Try to satisfy the read entirely from the buffer
    if (pos >= m_bufStart && pos + static_cast<int64_t>(length) <= m_bufStart + m_bufFill)
    {
      const size_t offset = static_cast<size_t>(pos - m_bufStart);
      TagLib::ByteVector bv(m_buf.data() + offset, static_cast<unsigned int>(length));
      m_file.Seek(pos + static_cast<int64_t>(length), SEEK_SET);
      return bv;
    }

    // For large reads that exceed the buffer size, bypass the buffer entirely
    if (length > kBufCapacity)
    {
      invalidateBuffer();
      TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
      ssize_t bytesRead = m_file.Read(bv.data(), length);
      if (bytesRead > 0)
        bv.resize(static_cast<unsigned int>(bytesRead));
      else
        bv.clear();
      return bv;
    }

    // Fill the buffer starting at the current file position
    fillBuffer(pos);

    const size_t avail = std::min(length, static_cast<size_t>(m_bufFill));
    if (avail == 0)
      return {};

    TagLib::ByteVector bv(m_buf.data(), static_cast<unsigned int>(avail));
    m_file.Seek(pos + static_cast<int64_t>(avail), SEEK_SET);
    return bv;
  }

  void writeBlock(const TagLib::ByteVector&) override {}
  void insert(const TagLib::ByteVector&, TagLib::offset_t, size_t) override {}
  void removeBlock(TagLib::offset_t, size_t) override {}
  bool readOnly() const override { return true; }

  void seek(TagLib::offset_t offset, TagLib::IOStream::Position p) override
  {
    int whence = SEEK_SET;
    if (p == TagLib::IOStream::Current)
      whence = SEEK_CUR;
    else if (p == TagLib::IOStream::End)
      whence = SEEK_END;
    m_file.Seek(offset, whence);
  }

  TagLib::offset_t tell() const override
  {
    return m_file.GetPosition();
  }

  TagLib::offset_t length() override
  {
    return m_fileLength;
  }

  void truncate(TagLib::offset_t) override {}
  void clear() override {}

  // Expose the underlying CFile for use by FFmpeg's AVIOContext
  XFILE::CFile& file() { return m_file; }

private:
  /*!
   * \brief Size of the internal read-ahead buffer.
   *
   * 64 KiB is large enough to absorb hundreds of TagLib's typical tiny
   * reads while small enough to avoid wasting memory. 131072 or 65536 recommended
   */
  static constexpr size_t kBufCapacity = 131072;

  void fillBuffer(int64_t filePos)
  {
    m_file.Seek(filePos, SEEK_SET);
    m_bufStart = filePos;
    ssize_t bytesRead = m_file.Read(m_buf.data(), kBufCapacity);
    m_bufFill = (bytesRead > 0) ? static_cast<size_t>(bytesRead) : 0;
    // Restore position to where it was; readBlock will advance it
    m_file.Seek(filePos, SEEK_SET);
  }

  void invalidateBuffer()
  {
    m_bufStart = -1;
    m_bufFill = 0;
  }

  std::string m_fileName;
  XFILE::CFile m_file;
  bool m_open = false;
  int64_t m_fileLength = 0;

  // Read-ahead buffer state
  std::vector<char> m_buf = std::vector<char>(kBufCapacity);
  int64_t m_bufStart = -1; //!< File offset where buffer contents begin
  size_t m_bufFill = 0;    //!< Number of valid bytes in the buffer
};