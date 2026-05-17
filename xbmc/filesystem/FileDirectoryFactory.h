/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IFileDirectory.h"

#include <string>

class CFileItem;
class CURL;

namespace XFILE
{
class CFileDirectoryFactory
{
public:
  CFileDirectoryFactory(void);
  virtual ~CFileDirectoryFactory(void);
  static IFileDirectory* Create(const CURL& url, CFileItem* pItem, const std::string& strMask="");
};

/*!
 * Return the number of song/chapter rows in the music DB for the file at this URL
 * (e.g. an audiobook with N pre-scanned chapters returns N, a CUE-expanded file
 * returns the number of CUE tracks). 0 if not yet in DB or DB unavailable.
 */
int GetChaptersCountInMusicDb(const CURL& url);

}
