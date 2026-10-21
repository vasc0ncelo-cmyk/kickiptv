#pragma once
#include <map>
#include <string>
#include <vector>

enum class Kind { Channel, Movie, Series };

struct Entry {
    std::string name, url, group, logo, tvgId;
    Kind kind = Kind::Channel;
    // so para series
    std::string seriesName;
    int season = 0, episode = 0;
};

struct Playlist {
    std::vector<Entry> channels, movies, series;
    // serie -> temporada -> episodios
    std::map<std::string, std::map<int, std::vector<Entry>>> seriesTree;
};

std::vector<Entry> parseM3U(const std::string &text);
Playlist buildPlaylist(const std::string &text);
bool parseEpisode(const std::string &name, std::string &series, int &season, int &episode);
