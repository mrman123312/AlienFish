/*
  AlienFish extensions to Stockfish. Copyright (C) 2026 AlienFish contributors.
  Distributed under the GNU General Public License, version 3 or later.
*/
#ifndef ALIEN_H_INCLUDED
#define ALIEN_H_INCLUDED

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "position.h"
#include "search.h"

namespace Stockfish::Alien {

struct Policy {
    int minDepth = 12;
    int minAdvantage = 0;  // UCI centipawns, from the side to move
    int maxLoss = 20;
    usize capacity = 100000;  // Move records, not positions
};

struct Record {
    std::string fen, move;
    bool chess960 = false;
    int cp = 0, bestCp = 0, depth = 0;
    u64 nodes = 0;
    bool removed = false;  // Retain deeper refutations as evidence, never as advice
};

// A position-indexed, bounded, append-only journal. Search uses only its memory
// index. File writes happen after bestmove, never in the recursive search.
class Legacy {
   public:
    bool configure(const std::filesystem::path&, const std::string& network, Policy);
    std::vector<std::string> hints(const Position&) const;
    void observe(const Position&, const Search::RootMoves&, u64 nodes, bool unrestricted,
                 const std::vector<std::string>& allowed = {});
    std::string flush();
    std::string compact();
    std::string status() const;
    usize size() const { return recordCount; }

   private:
    void load();
    bool admit(const Record&) const;
    void apply(const Record&, bool remove);
    std::filesystem::path file;
    std::string model;
    Policy policy;
    bool configured = false, writable = true, indexLimited = false;
    usize recordCount = 0, evidenceCount = 0, rejected = 0;
    std::unordered_map<std::string, std::vector<Record>> bank;
    std::vector<std::pair<Record, bool>> pending;
    std::string error;
};

std::string position_key(const Position&);  // Includes halfmove clock and variant
std::string json_string(const std::string&);
std::string candidates_json(const Position&, const Search::RootMoves&, int depth);
std::string gambit_move(const Position&);  // White's moves; opponent cooperation is required
int sacrifice_score(const Position&, const Search::RootMove&, int horizon);
Move select_brilliant(const Position&, const Search::RootMoves&, int minDepth,
                      int maxLoss, int horizon);

}  // namespace Stockfish::Alien
#endif
