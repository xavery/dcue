// *******************************************************************
// DCue (github.com/xavery/dcue)
// Copyright (c) 2019-2022 Daniel Kamil Kozar
// Original version by :
// DCue (sourceforge.net/projects/dcue)
// Copyright (c) 2013 Fluxtion, DCue project
// *******************************************************************
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
// *******************************************************************

#include "cue.h"

#include "defs.h"
#include "naming.h"
#include "string_utility.h"
#include "support_types.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <optional>

#include <spdlog/fmt/fmt.h>

namespace {
std::string sanitise_string(const std::string& str) {
  auto out = str;
  replace_string(out, "\\u2019", "\'");
  replace_string(out, "\\u2026", "...");
  replace_string(out, "\\\"", "\'");
  replace_char(out, '\\', "-");
  replace_char(out, '/', "-");
  return out;
}

void open_file(std::ofstream& out, const std::filesystem::path& fpath,
               unsigned disc = 0) {
  auto cuepath = fpath;
  if (disc != 0) {
    const auto disc_str = std::to_string(disc);
    auto fname = cuepath.stem().string();
    if (!replace_char(fname, '?', disc_str)) {
      cuepath.replace_filename(fname + '-' + disc_str);
    }
  }

  cuepath.replace_extension(".cue");

  /* the output stream that the contents of the CUE are written into is opened
   * in binary mode in order to preserve CRLF line endings. */
  out.open(cuepath, std::ios::binary | std::ios::out);
  if (!out.is_open()) {
    throw std::runtime_error(
        fmt::format("Cannot open output file! (\"{}\")", cuepath.string()));
  }
}

std::string concatenate_artists(const nlohmann::json& artists) {
  std::string rv;
  for (auto&& artist_info : artists) {
    auto name = artist_info.value("anv", std::string());
    if (name.empty()) {
      name = artist_info.value("name", std::string());
    }
    NamingFacets::artist_facets(name);
    rv += name;
    if (&artist_info != &artists.back()) {
      auto join = artist_info.value("join", ",");
      if (join != ",") {
        rv += " ";
      }
      rv += join;
      rv += " ";
    }
  }
  return rv;
}

std::optional<unsigned> get_disc_number(const std::string& position) {
  auto dotpos = position.find_first_of(".-");
  if (dotpos == std::string::npos || (dotpos + 1) == position.length()) {
    return std::nullopt;
  }
  auto pre_dot = std::string_view{position}.substr(0, dotpos);
  auto post_dot = std::string_view{position}.substr(dotpos + 1);
  unsigned discno;
  auto pre_dot_result =
      std::from_chars(pre_dot.data(), pre_dot.data() + pre_dot.size(), discno);
  unsigned dummy;
  auto post_dot_result = std::from_chars(
      post_dot.data(), post_dot.data() + post_dot.size(), dummy);
  if (pre_dot_result.ec == std::errc{} && post_dot_result.ec == std::errc{}) {
    return discno;
  } else {
    return std::nullopt;
  }
}

class Cue {
  std::ostream& stream;

  void add_meta(const std::string& comment) {
    stream << "REM " << comment << "\r\n";
  }
  void add_generic_time(const unsigned minutes, const unsigned seconds,
                        const unsigned frames) {
    stream << std::setfill('0') << std::setw(2) << minutes << ':'
           << std::setfill('0') << std::setw(2) << seconds << ':'
           << std::setfill('0') << std::setw(2) << frames;
  }
  void add_index(const char* index, unsigned minutes, unsigned seconds) {
    stream << "INDEX " << index << ' ';
    /* Discogs timestamps aren't detailed enough to provide CD frame accuracy,
     * so we just go with 0 here. */
    add_generic_time(minutes, seconds, 0);
    stream << "\r\n";
  }
  void add_type_from_ext(const std::filesystem::path& fpath) {
    auto extension = fpath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   ::tolower);
    if (extension == ".mp3") {
      stream << "MP3";
    } else if (extension == ".aiff") {
      stream << "AIFF";
    } else {
      stream << "WAVE";
    }
  }

public:
  void add_genre(const std::string& genre) {
    add_meta("GENRE " + genre);
  }
  void add_year(const std::string& year) {
    add_meta("DATE " + year);
  }
  void add_comment(const std::string& comment) {
    add_meta("COMMENT \"" + comment + "\"");
  }
  void add_artist(const std::string& artist) {
    stream << "PERFORMER " << std::quoted(artist) << "\r\n";
  }
  void add_title(const std::string& title) {
    stream << "TITLE " << std::quoted(title) << "\r\n";
  }
  void add_track(const unsigned num) {
    stream << "TRACK " << std::setfill('0') << std::setw(2) << num
           << " AUDIO\r\n";
  }
  void add_track_index(const unsigned minutes, const unsigned seconds) {
    add_index("01", minutes, seconds);
  }
  void add_filename(const std::filesystem::path& fpath, unsigned discno) {
    auto name = fpath.string();
    replace_char(name, '?', std::to_string(discno));
    stream << "FILE " << std::quoted(name) << ' ';
    add_type_from_ext(fpath);
    stream << "\r\n";
  }
  void add_indent() {
    stream << '\t';
  }
  Cue(std::ostream& os) : stream(os) {
  }
};

void Cue_build(const Album& album, const std::filesystem::path& fpath) {
  for (auto i = 0u; i < album.discs.size(); ++i) {
    auto&& disc = album.discs[i];
    auto discno = i + 1;
    std::ofstream f;
    if (album.discs.size() > 1) {
      open_file(f, fpath, discno);
    } else {
      open_file(f, fpath);
    }
    Cue c(f);
    // string sanitisation is really just a way of compensating for the number
    // of dumb cue tools available, double quotes especially confuse them and
    // Medieval CUE Splitter on Windows practically blows up when confronted
    // with backslashes in titles  it's not really the application's job to do
    // this (bar perhaps the double quotes) but because there's no actual
    // standard for cue sheets we have to make do and mend
    if (!album.genre.empty()) {
      c.add_genre(sanitise_string(album.genre));
    }
    if (!album.year.empty()) {
      c.add_year(album.year);
    }
    c.add_comment(COMMENT);
    if (!album.album_artist.empty()) {
      c.add_artist(sanitise_string(album.album_artist));
    }
    if (!album.title.empty()) {
      c.add_title(sanitise_string(album.title));
    }
    if (!fpath.empty()) {
      c.add_filename(fpath, discno);
    }
    Track::Duration total;
    for (auto&& track : disc.tracks) {
      c.add_indent();
      c.add_track(track.position);
      c.add_indent();
      c.add_indent();
      if (!track.title.empty()) {
        c.add_title(sanitise_string(track.title));
      }
      c.add_indent();
      c.add_indent();
      if (!track.artist.empty()) {
        c.add_artist(sanitise_string(track.artist));
      }
      c.add_indent();
      c.add_indent();
      c.add_track_index(total.min, total.sec);
      total += track.length;
    }
  }
}

Track::Duration parse_duration(std::string_view dur) {
  const auto colon_pos = dur.find(':');
  if (colon_pos == std::string_view::npos || colon_pos == dur.length() - 1) {
    throw std::runtime_error(
        fmt::format("Unrecognised duration {}, qutting", dur));
  }
  unsigned min, sec;
  auto min_conv_result =
      std::from_chars(dur.data(), dur.data() + colon_pos, min);
  auto sec_conv_result = std::from_chars(dur.data() + colon_pos + 1,
                                         dur.data() + dur.length(), sec);
  if (min_conv_result.ec == std::errc{} && sec_conv_result.ec == std::errc{}) {
    return Track::Duration{min, sec};
  } else {
    throw std::runtime_error(
        fmt::format("Unrecognised duration {}, qutting", dur));
  }
}

}

void generate(const nlohmann::json& toplevel,
              const std::filesystem::path& fpath) {
  Album a;
  a.title = toplevel.value("title", std::string());
  auto year = toplevel.value("year", -1);
  if (year != -1) {
    a.year = std::to_string(year);
  }
  // style maps to genre better than genre does, in general
  if (toplevel.find("styles") != toplevel.end()) {
    a.genre = toplevel["styles"][0].get<std::string>();
  }
  if (toplevel.find("artists") != toplevel.end()) {
    a.album_artist = concatenate_artists(toplevel["artists"]);
  }

  Disc d;
  a.discs.push_back(d);
  unsigned disc = 0;
  unsigned track_num = 1;
  for (auto&& track_info : toplevel.at("tracklist")) {
    auto position = track_info.value("position", std::string());
    if (position.empty()) {
      continue;
    }
    const auto this_disc = get_disc_number(position);
    if (this_disc && *this_disc > disc) {
      ++disc;
      Disc nd;
      a.discs.push_back(nd);
      track_num = 1;
    }
    Track t;
    t.position = track_num;
    if (track_info.find("artists") != track_info.end()) {
      t.artist = concatenate_artists(track_info["artists"]);
    } else {
      t.artist = a.album_artist;
    }
    t.title = track_info.value("title", std::string());
    auto duration = track_info.value("duration", std::string());
    if (duration.empty()) {
      throw std::runtime_error(fmt::format(
          "Track {}, disc {} has no duration : quitting", track_num, disc));
    }
    t.length = parse_duration(duration);
    ++track_num;
    a.discs[disc].tracks.push_back(t);
  }

  // if multidisc album, we have to remove the useless disc we created in the
  // loop
  if (a.discs.size() > 1) {
    a.discs.erase(a.discs.begin());
  }

  Cue_build(a, fpath);
}
