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
#include <string>

/*!
 * \brief VFS-backed TagLib IOStream adapter.
 *
 * Allows TagLib to read through Kodi's virtual filesystem
 * (supports nfs://, smb://, etc.)
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
    return m_open;
  }

  TagLib::ByteVector readBlock(size_t length) override
  {
    TagLib::ByteVector bv(static_cast<unsigned int>(length), 0);
    ssize_t read = m_file.Read(bv.data(), length);
    if (read > 0)
      bv.resize(static_cast<unsigned int>(read));
    else
      bv.clear();
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
    return m_file.GetLength();
  }

  void truncate(TagLib::offset_t) override {}
  void clear() override {}

  // Expose the underlying CFile for use by FFmpeg's AVIOContext
  XFILE::CFile& file() { return m_file; }

private:
  std::string m_fileName;
  XFILE::CFile m_file;
  bool m_open = false;
};