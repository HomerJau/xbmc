/*
 *  Copyright (C) 2005-2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "filesystem/File.h"
#include <taglib/tiostream.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

/*!
 * \brief VFS-backed TagLib IOStream adapter with read-ahead buffering.
 *
 * Allows TagLib to read through Kodi's virtual filesystem
 * (supports nfs://, smb://, etc.)
 *
 * TagLib's EBML parser makes many small reads (1-8 bytes for element IDs
 * and sizes). On network filesystems each read is a round-trip. The
 * read-ahead buffer batches these into larger reads (default 64KB) to
 * dramatically reduce NFS/SMB latency.
 */
class KodiTagLibStream : public TagLib::IOStream
{
public:
  static constexpr size_t BUFFER_SIZE = 65536; // 64KB read-ahead

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
      m_buffer.resize(BUFFER_SIZE);
      m_bufferFill = 0;
      m_bufferPos = 0;
    }
    return m_open;
  }

  TagLib::ByteVector readBlock(size_t length) override
  {
    if (length == 0)
      return TagLib::ByteVector();

    // For very large reads (e.g. cover art data), bypass the buffer
    if (length > BUFFER_SIZE)
    {
      invalidateBuffer();
      TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
      ssize_t read = m_file.Read(bv.data(), length);
      if (read > 0)
        bv.resize(static_cast<unsigned int>(read));
      else
        bv.clear();
      return bv;
    }

    TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
    size_t totalRead = 0;
    char* dest = bv.data();

    while (totalRead < length)
    {
      if (m_bufferFill > 0 && m_bufferPos < m_bufferFill)
      {
        // Serve from buffer
        size_t available = m_bufferFill - m_bufferPos;
        size_t toCopy = std::min(available, length - totalRead);
        std::memcpy(dest + totalRead, m_buffer.data() + m_bufferPos, toCopy);
        m_bufferPos += toCopy;
        totalRead += toCopy;
      }
      else
      {
        // Buffer exhausted — refill from file
        ssize_t read = m_file.Read(m_buffer.data(), BUFFER_SIZE);
        if (read <= 0)
          break;
        m_bufferFill = static_cast<size_t>(read);
        m_bufferPos = 0;
      }
    }

    if (totalRead < length)
      bv.resize(static_cast<unsigned int>(totalRead));

    return bv;
  }

  void writeBlock(const TagLib::ByteVector&) override {}
  void insert(const TagLib::ByteVector&, TagLib::offset_t, size_t) override {}
  void removeBlock(TagLib::offset_t, size_t) override {}
  bool readOnly() const override { return true; }

  void seek(TagLib::offset_t offset, TagLib::IOStream::Position p) override
  {
    if (p == TagLib::IOStream::Current)
    {
      // For relative seeks within the buffer, just adjust position
      if (m_bufferFill > 0)
      {
        int64_t newPos = static_cast<int64_t>(m_bufferPos) + offset;
        if (newPos >= 0 && newPos <= static_cast<int64_t>(m_bufferFill))
        {
          m_bufferPos = static_cast<size_t>(newPos);
          return;
        }
      }
      // Outside buffer — convert to absolute seek
      offset = tell() + offset;
      p = TagLib::IOStream::Beginning;
    }

    if (p == TagLib::IOStream::Beginning && m_bufferFill > 0)
    {
      // Check if target is within the current buffer
      int64_t filePos = m_file.GetPosition();
      int64_t bufferStart = filePos - static_cast<int64_t>(m_bufferFill);
      int64_t bufferEnd = filePos;

      if (offset >= bufferStart && offset < bufferEnd)
      {
        m_bufferPos = static_cast<size_t>(offset - bufferStart);
        return; // seek satisfied from buffer — no NFS round-trip
      }
    }

    // Outside buffer — do a real seek and invalidate
    invalidateBuffer();
    int whence = SEEK_SET;
    if (p == TagLib::IOStream::End)
      whence = SEEK_END;
    m_file.Seek(offset, whence);
  }

  TagLib::offset_t tell() const override
  {
    // Adjust for buffered data not yet consumed
    if (m_bufferFill > 0)
      return m_file.GetPosition() - static_cast<TagLib::offset_t>(m_bufferFill - m_bufferPos);
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
  void invalidateBuffer()
  {
    if (m_bufferFill > 0 && m_bufferPos < m_bufferFill)
    {
      // Seek the file back to the logical position so the next
      // real read starts from where the caller expects
      int64_t unread = static_cast<int64_t>(m_bufferFill - m_bufferPos);
      m_file.Seek(-unread, SEEK_CUR);
    }
    m_bufferFill = 0;
    m_bufferPos = 0;
  }

  std::string m_fileName;
  XFILE::CFile m_file;
  bool m_open = false;
  int64_t m_fileLength = 0;

  // Read-ahead buffer
  std::vector<char> m_buffer;
  size_t m_bufferFill = 0; // how many bytes are valid in the buffer
  size_t m_bufferPos = 0;  // current read position within the buffer
};