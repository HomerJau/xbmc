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
 *
 * For Matroska files, TagLib walks every top-level EBML element (including
 * huge Cluster elements containing media data) by reading a short element
 * header, then seeking past the element body.  On a multi-GB file over
 * NFS/SMB each of those tiny reads at a new offset triggers a VFS
 * round-trip.  The virtual file position tracking here eliminates redundant
 * VFS seeks: the real CFile seek is deferred until data is actually read,
 * so consecutive seek-then-seek or seek-past-buffered-data chains cost
 * nothing on the network.
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

    const int64_t pos = m_virtualPos;

    // Try to satisfy the read entirely from the buffer
    if (pos >= m_bufStart &&
        pos + static_cast<int64_t>(length) <= m_bufStart + static_cast<int64_t>(m_bufFill))
    {
      const size_t offset = static_cast<size_t>(pos - m_bufStart);
      TagLib::ByteVector bv(m_buf.data() + offset, static_cast<unsigned int>(length));
      m_virtualPos = pos + static_cast<int64_t>(length);
      return bv;
    }

    // For large reads that exceed the buffer size, bypass the buffer entirely
    if (length > kBufCapacity)
    {
      invalidateBuffer();
      syncFilePosition(pos);
      TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
      ssize_t bytesRead = m_file.Read(bv.data(), length);
      if (bytesRead > 0)
      {
        bv.resize(static_cast<unsigned int>(bytesRead));
        m_virtualPos = pos + bytesRead;
        m_filePos = m_virtualPos;
      }
      else
        bv.clear();
      return bv;
    }

    // Fill the buffer starting at the current virtual position
    fillBuffer(pos);

    const size_t avail = std::min(length, static_cast<size_t>(m_bufFill));
    if (avail == 0)
      return {};

    TagLib::ByteVector bv(m_buf.data(), static_cast<unsigned int>(avail));
    m_virtualPos = pos + static_cast<int64_t>(avail);
    return bv;
  }

  void writeBlock(const TagLib::ByteVector&) override {}
  void insert(const TagLib::ByteVector&, TagLib::offset_t, size_t) override {}
  void removeBlock(TagLib::offset_t, size_t) override {}
  bool readOnly() const override { return true; }

  /*!
   * \brief Seek to a new position — updates only the virtual position.
   *
   * The actual CFile::Seek is deferred until the next readBlock() or
   * fillBuffer() call.  This is critical for Matroska parsing where TagLib
   * performs thousands of seek-read-seek cycles to skip past Cluster
   * elements.  Many of those seeks are followed by another seek (when the
   * element is skipped) or land inside the existing buffer, so deferring
   * avoids thousands of NFS/SMB round-trips.
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

  TagLib::offset_t tell() const override
  {
    return m_virtualPos;
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
   * 128 KiB is large enough to absorb hundreds of TagLib's typical tiny
   * reads while small enough to avoid wasting memory.
   */
  static constexpr size_t kBufCapacity = 131072;

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

  void fillBuffer(int64_t filePos)
  {
    syncFilePosition(filePos);
    m_bufStart = filePos;
    ssize_t bytesRead = m_file.Read(m_buf.data(), kBufCapacity);
    m_bufFill = (bytesRead > 0) ? static_cast<size_t>(bytesRead) : 0;
    m_filePos = filePos + static_cast<int64_t>(m_bufFill);
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

  // Virtual file position — may diverge from the real CFile position
  // between seek() and the next readBlock(). This avoids costly VFS
  // seeks when TagLib seeks repeatedly without reading.
  int64_t m_virtualPos = 0;

  // Tracks the real CFile position so we can skip redundant Seek calls
  int64_t m_filePos = 0;

  // Read-ahead buffer state
  std::vector<char> m_buf = std::vector<char>(kBufCapacity);
  int64_t m_bufStart = -1; //!< File offset where buffer contents begin
  size_t m_bufFill = 0;    //!< Number of valid bytes in the buffer
};