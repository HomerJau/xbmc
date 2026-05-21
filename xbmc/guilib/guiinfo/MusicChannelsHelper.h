/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/StringUtils.h"

#include <string>

namespace KODI::GUILIB::GUIINFO
{

/*!
 * Map (codec, channel count, path) to a friendly channel-layout label
 * for skins. Three layers, checked in order:
 *
 *  1. Codec-based labels (Atmos, DTS:X) — unambiguous, win first.
 *  2. Path-based hints. Recover collection-specific intent that the
 *     raw channel count cannot express. The motivating case is 70s
 *     Quad music: many users store these files silent-channel-padded
 *     to 5.0 / 5.1 / 6.1 so legacy AVRs and older players recognise
 *     them as multichannel instead of down-mixing to stereo. Without
 *     a path hint, the channel-count switch below would label these
 *     "5.0" / "5.1" / "6.1" when they're really Quad.
 *  3. Standard channel-count → label mapping for everything else.
 *
 * Returns an empty string when no rule applies — skins can fall back
 * to the raw codec or channel-count InfoLabels.
 *
 * The `path` argument is the file's full path (caller supplies the
 * canonical music URL with a fallback to the item path). Hint rules
 * match against the entire path so parent folder names ("Quad/",
 * "Quadio/") feed into the decision.
 *
 * Shared by ListItem.MusicChannelsString (music library browsers),
 * MusicPlayer.MusicChannelsString (PaPlayer now-playing), and
 * VideoPlayer.MusicChannelsString (VideoPlayer now-playing — used
 * when concert MKVs or audio-via-VideoPlayer items are active).
 */
inline std::string MakeMusicChannelsString(const std::string& codec,
                                           int channels,
                                           const std::string& path)
{
  // --- Layer 1: codec-based labels ----------------------------------------
  if (codec == "eac3_ddp_atmos" || codec == "truehd_atmos")
    return "Atmos";
  if (codec == "dtshd_ma_x")
    return "DTS:X";

  // --- Layer 2: path-based hints ------------------------------------------
  //
  // Order matters — more-specific patterns must come BEFORE general ones,
  // because the first matching rule wins.
  //
  // Quad-padded-with-silence detection: many 70s Quad files are stored
  // silent-padded to 5ch / 6ch so legacy AVRs and older players recognise
  // them as multichannel instead of down-mixing to stereo; the real intent
  // is "Quad", not "5.0" / "5.1". Without these rules the channel-count
  // switch below would mis-label them.
  //
  // Gate on channels in {4,5,6} so true 5.1 / 7.1 multichannel skip
  // these rules entirely.
  if (channels >= 4 && channels <= 6)
  {
    // "Quad+" : Quad source with LFE (4 active channels + LFE channel),
    // stored as 5ch (Quad + LFE) or 6ch (Quad + LFE + silent rear). The
    // "+" marker is distinctive enough that a bare substring match is
    // safe — no folder-boundary slash needed, and no realistic album
    // name contains the literal "Quad+" by accident.
    //
    // Runs BEFORE the Penteo rule so a folder like
    // "Aerosmith - Aerosmith (Quad+ Penteo UM)" returns "Quad+" rather
    // than the Penteo upmix label.
    if ((channels == 5 || channels == 6) &&
        path.find("Quad+") != std::string::npos)
      return "Quad+";

    // Penteo upmix of a Quad source. Garry's collection labels these
    // "4.1 UM" when 6ch (Quad + LFE + silent) and "Quad UM" when 5ch
    // (Quad + silent rear).
    //
    // Use "Quad/" with the trailing slash so this matches only the literal
    // parent folder "Quad/" and NOT album names that happen to start with
    // "Quad" (e.g. The Who's "Quadrophenia", "Quadromania", "Quadrille"
    // etc. would false-positive otherwise).
    if (path.find("Penteo") != std::string::npos &&
        path.find("Quad/")  != std::string::npos)
    {
      return channels == 6 ? "4.1 UM" : "Quad UM";
    }

    // "Quadio" branding — a specific Quad label/series; show its name.
    if (path.find("Quadio") != std::string::npos)
      return "Quadio";

    // Generic Quad folder hint (e.g. .../Quad/<artist>/<album>/...) or
    // a "4.0" tag anywhere in the path.
    if (path.find("Quad/") != std::string::npos ||
        path.find("4.0")   != std::string::npos)
      return "Quad";
  }

  // --- Layer 3: standard channel-count → label ----------------------------
  switch (channels)
  {
    case 1: return "Mono";
    case 2: return "Stereo";
    case 3: return "3.0";
    case 4: return "Quad";
    case 5: return "5.0";
    case 6: return "5.1";
    case 7: return "6.1";
    case 8: return "7.1";
    default: break;
  }
  if (channels >= 9)
    return StringUtils::Format("{}.0", channels);
  return {};
}

} // namespace KODI::GUILIB::GUIINFO
