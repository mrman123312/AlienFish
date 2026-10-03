/*
  AlienFish extensions to Stockfish. Copyright (C) 2026 AlienFish contributors.
  Distributed under the GNU General Public License, version 3 or later.
*/
#include "alien.h"

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include "movegen.h"
#include "uci.h"

namespace Stockfish::Alien {
namespace {

std::string normalized_fen(const Position& pos) {
    std::istringstream in(pos.fen());
    std::string token, fen;
    for (int i = 0; i < 5 && in >> token; ++i)
        fen += (i ? " " : "") + token;
    return fen + " 1";  // Fullmove number is irrelevant to this advice index
}

std::string key(const Record& r) { return (r.chess960 ? "960 " : "std ") + r.fen; }

u64 checksum(const std::string& s) {
    u64 h = 14695981039346656037ULL;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ULL;
    return h;
}

std::string encode(const Record& r, bool remove) {
    std::ostringstream out;
    out << (remove ? 'D' : 'P') << ' ' << std::quoted(r.fen) << ' ' << r.chess960
        << ' ' << std::quoted(r.move) << ' ' << r.cp << ' ' << r.bestCp << ' '
        << r.depth << ' ' << r.nodes;
    const auto body = out.str();
    out << ' ' << std::hex << checksum(body) << '\n';
    return out.str();
}

bool decode(const std::string& line, Record& r, bool& remove) {
    if (line.size() > 1024)
        return false;
    auto split = line.rfind(' ');
    if (split == std::string::npos)
        return false;
    const auto body = line.substr(0, split);
    u64 check = 0;
    std::istringstream hash(line.substr(split + 1));
    hash >> std::hex >> check;
    if (!hash || !(hash >> std::ws).eof() || check != checksum(body))
        return false;
    char kind;
    int variant;
    std::istringstream in(body);
    in >> kind >> std::quoted(r.fen) >> variant >> std::quoted(r.move) >> r.cp
       >> r.bestCp >> r.depth >> r.nodes;
    if (!in || !(in >> std::ws).eof() || (kind != 'P' && kind != 'D')
        || (variant != 0 && variant != 1) || r.move.size() < 4 || r.move.size() > 5
        || r.depth < 1 || r.depth >= MAX_PLY || r.cp < -20000 || r.cp > 20000
        || r.bestCp < -20000 || r.bestCp > 20000 || r.bestCp < r.cp)
        return false;
    r.chess960 = variant;
    Position pos;
    StateInfo st;
    if (pos.set(r.fen, r.chess960, &st) || normalized_fen(pos) != r.fen
        || UCIEngine::to_move(pos, r.move) == Move::none())
        return false;
    remove = kind == 'D';
    return true;
}

bool header_ok(std::istream& in, const std::string& model) {
    std::string line, magic, network;
    int version = 0;
    if (!std::getline(in, line) || line.size() > 2048)
        return false;
    std::istringstream header(line);
    header >> magic >> version >> std::quoted(network);
    return header && (header >> std::ws).eof() && magic == "AlienLegacy"
        && version == 1 && network == model;
}

// An atomic directory creation serializes append/compaction across processes.
// A crash can leave this directory behind; status reports it instead of
// risking concurrent writes. Users can remove a stale lock after stopping writers.
struct FileLock {
    explicit FileLock(const std::filesystem::path& file) : path(file) {
        path += ".lock";
        std::error_code ec;
        held = std::filesystem::create_directory(path, ec);
    }
    ~FileLock() {
        if (held) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    std::filesystem::path path;
    bool held = false;
};

int material_balance(const Position& pos, Color us) {
    constexpr std::array<int, PIECE_TYPE_NB> weights = {0, 100, 320, 330, 500, 900, 0, 0};
    int score = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        Piece p = pos.piece_on(s);
        if (p != NO_PIECE)
            score += weights[type_of(p)] * (color_of(p) == us ? 1 : -1);
    }
    return score;
}

}  // namespace

std::string position_key(const Position& pos) {
    return (pos.is_chess960() ? "960 " : "std ") + normalized_fen(pos);
}

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << char(c);
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
        else
            out << char(c);
    }
    return out.str() + '"';
}

bool Legacy::configure(const std::filesystem::path& path, const std::string& network, Policy p) {
    const bool capacityChanged = p.capacity != policy.capacity;
    policy = p;
    if (configured && path == file && network == model && !capacityChanged)
        return writable;
    if (!pending.empty() && !flush().empty())
        return false;  // Preserve unsaved records and their original network identity
    file = path;
    model = network;
    configured = true;
    load();
    return writable;
}

void Legacy::load() {
    bank.clear();
    pending.clear();
    recordCount = evidenceCount = rejected = 0;
    error.clear();
    writable = true;
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) {
        if (ec) { error = "Cannot inspect AlienLegacy file: " + ec.message(); writable = false; }
        return;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in || !header_ok(in, model)) {
        error = "AlienLegacy header/network mismatch or unreadable file; file preserved";
        writable = false;
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        Record r;
        bool remove;
        if (!decode(line, r, remove)) { ++rejected; continue; }
        apply(r, remove);
    }
    if (in.bad()) { error = "AlienLegacy read error"; writable = false; }
}

bool Legacy::admit(const Record& r) const {
    return r.depth >= policy.minDepth && r.cp >= policy.minAdvantage
        && r.bestCp >= r.cp && r.bestCp - r.cp <= policy.maxLoss;
}

void Legacy::apply(const Record& r, bool remove) {
    const auto k = key(r);
    auto it = bank.find(k);
    if (it == bank.end()) {
        if (evidenceCount >= policy.capacity)
            return;
        it = bank.emplace(k, std::vector<Record>{}).first;
    }
    auto& records = it->second;
    auto found = std::find_if(records.begin(), records.end(), [&](const auto& old) {
        return old.move == r.move;
    });
    if (found != records.end()) {
        if (r.depth < found->depth || (r.depth == found->depth && r.nodes < found->nodes))
            return;
        recordCount += usize(!remove);
        recordCount -= usize(!found->removed);
        *found = r;
        found->removed = remove;
    }
    else if (evidenceCount < policy.capacity) {
        records.push_back(r);
        records.back().removed = remove;
        ++evidenceCount;
        recordCount += usize(!remove);
    }
}

std::vector<std::string> Legacy::hints(const Position& pos) const {
    if (!writable || pos.has_repeated())
        return {};
    auto it = bank.find(position_key(pos));
    if (it == bank.end())
        return {};
    auto records = it->second;
    std::stable_sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        if (a.cp != b.cp) return a.cp > b.cp;
        if (a.depth != b.depth) return a.depth > b.depth;
        return a.move < b.move;
    });
    std::vector<std::string> moves;
    for (const auto& r : records)
        if (!r.removed && admit(r) && UCIEngine::to_move(pos, r.move) != Move::none())
            moves.push_back(r.move);
    return moves;
}

void Legacy::observe(const Position& pos, const Search::RootMoves& moves, u64 nodes, bool unrestricted,
                     const std::vector<std::string>& allowed) {
    if (!writable || file.empty() || !unrestricted || pos.has_repeated() || moves.empty()
        || moves.front().is_inexact() || is_decisive(moves.front().score)
        || moves.front().verifiedDepth < policy.minDepth)
        return;
    const int bestCp = UCIEngine::to_cp(moves.front().score, pos);
    for (const auto& rm : moves) {
        if (rm.pv.empty() || rm.is_inexact() || is_decisive(rm.score)
            || rm.verifiedDepth < policy.minDepth || pending.size() >= 4096)
            continue;
        Record r{normalized_fen(pos), UCIEngine::move(rm.pv[0], pos.is_chess960()),
                 pos.is_chess960(), UCIEngine::to_cp(rm.score, pos), bestCp, rm.verifiedDepth, nodes};
        if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), r.move) == allowed.end())
            continue;
        if (r.cp > bestCp)
            continue;
        auto it = bank.find(key(r));
        auto old = it == bank.end() ? std::vector<Record>{} : it->second;
        auto found = std::find_if(old.begin(), old.end(), [&](const auto& x) { return x.move == r.move; });
        if (found != old.end() && (r.depth < found->depth
            || (r.depth == found->depth && r.nodes <= found->nodes)))
            continue;
        const bool remove = !admit(r);
        if ((remove && found == old.end()) || (!remove && found == old.end()
            && evidenceCount >= policy.capacity))
            continue;
        apply(r, remove);
        pending.emplace_back(r, remove);
    }
}

std::string Legacy::flush() {
    if (pending.empty())
        return error;
    if (!writable)
        return error;
    std::error_code ec;
    if (!file.parent_path().empty())
        std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return error = "AlienLegacy directory error: " + ec.message();
    FileLock lock(file);
    if (!lock.held) return error = "AlienLegacy writer lock busy or inaccessible; records remain pending";
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) return error = "AlienLegacy file error: " + ec.message();
    if (exists) {
        std::ifstream in(file, std::ios::binary);
        if (!in || !header_ok(in, model))
            return error = "AlienLegacy file changed or network mismatch; records remain pending";
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return error = "Cannot write AlienLegacy file; records remain pending";
    if (!exists)
        out << "AlienLegacy 1 " << std::quoted(model) << '\n';
    // Separate any torn final record left by a crash. Its checksum will fail
    // on load; all complete records before and after it remain recoverable.
    out << '\n';
    for (const auto& entry : pending)
        out << encode(entry.first, entry.second);
    out.flush();
    if (!out) return error = "AlienLegacy write failed; records remain pending";
    pending.clear();
    error.clear();
    return {};
}

std::string Legacy::compact() {
    if (auto message = flush(); !message.empty()) return message;
    if (!configured || file.empty() || !writable) return "AlienLegacy file unavailable";
    FileLock lock(file);
    if (!lock.held) return "AlienLegacy writer lock busy or inaccessible";
    // Include other writers' completed appends before replacing the journal.
    load();
    if (!writable) return error;
    auto temp = file;
    temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return "Cannot create AlienLegacy compact file";
    out << "AlienLegacy 1 " << std::quoted(model) << '\n';
    std::vector<Record> records;
    for (const auto& bucket : bank)
        for (const auto& r : bucket.second) records.push_back(r);
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return key(a) == key(b) ? a.move < b.move : key(a) < key(b);
    });
    for (const auto& r : records) out << encode(r, r.removed);
    out.flush();
    if (!out) return "AlienLegacy compaction write failed; original preserved";
    out.close();
    std::error_code ec;
#if defined(_WIN32)
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(int(GetLastError()), std::system_category());
#else
    std::filesystem::rename(temp, file, ec);
#endif
    if (ec) return "AlienLegacy atomic replacement unavailable: " + ec.message() + "; original preserved";
    return {};
}

std::string Legacy::status() const {
    std::ostringstream out;
    const auto utf8 = file.u8string();
    const auto positions = std::count_if(bank.begin(), bank.end(), [](const auto& bucket) {
        return std::any_of(bucket.second.begin(), bucket.second.end(), [](const auto& r) {
            return !r.removed;
        });
    });
    out << "{\"file\":" << json_string(std::string(utf8.begin(), utf8.end())) << ",\"network\":" << json_string(model)
        << ",\"positions\":" << positions << ",\"records\":" << recordCount
        << ",\"refutations\":" << evidenceCount - recordCount
        << ",\"pending\":" << pending.size() << ",\"rejected\":" << rejected
        << ",\"capacity\":" << policy.capacity << ",\"error\":" << json_string(error) << '}';
    return out.str();
}

std::string gambit_move(const Position& root) {
    if (root.is_chess960() || root.side_to_move() != WHITE || root.has_repeated())
        return {};
    // Canonical Caro-Kann Alien Gambit, including the Nd2 transposition.
    // Compare board/turn/castling/en-passant; move counters do not define openings.
    auto opening_key = [](const Position& p) {
        auto fen = normalized_fen(p);
        return fen.substr(0, fen.rfind(' ', fen.rfind(' ') - 1));
    };
    const auto target = opening_key(root);
    for (bool nd2 : {false, true}) {
        const std::array<std::string, 13> line = {
          "e2e4", "c7c6", "d2d4", "d7d5", nd2 ? "b1d2" : "b1c3", "d5e4",
          nd2 ? "d2e4" : "c3e4", "g8f6", "e4g5", "h7h6", "g5f7", "e8f7", "g1f3"};
        Position pos;
        std::deque<StateInfo> states(1);
        pos.set(StartFEN, false, &states.back());
        for (usize i = 0; i < line.size(); ++i) {
            if (i % 2 == 0 && opening_key(pos) == target)
                return line[i];
            auto move = UCIEngine::to_move(pos, line[i]);
            if (move == Move::none()) break;
            states.emplace_back();
            pos.do_move(move, states.back(), nullptr);
        }
    }
    return {};
}

int sacrifice_score(const Position& root, const Search::RootMove& rm, int horizon) {
    Position pos;
    std::deque<StateInfo> states(1);
    if (pos.set(root.fen(), root.is_chess960(), &states.back())) return 0;
    const Color us = root.side_to_move();
    struct Offer { int ply, balance; bool seeNegative, accepted; };
    std::vector<Offer> offers;
    std::vector<int> balance;
    const usize length = std::min(rm.pv.size(), usize(horizon));
    for (usize i = 0; i < length; ++i) {
        Move m = rm.pv[i];
        const MoveList<LEGAL> legal(pos);
        if (std::find(legal.begin(), legal.end(), m) == legal.end()) {
            return 0;
        }
        if (i % 2 == 0)
            offers.push_back({int(i), material_balance(pos, us),
                              m.type_of() != CASTLING && type_of(pos.moved_piece(m)) != KING
                              && !pos.see_ge(m, -100), false});
        else if (!offers.empty() && pos.capture(m) && pos.see_ge(m, 100))
            // The investment may be a different piece: e.g. moving a pinned
            // knight and allowing a bishop to take our queen in Legal's motif.
            offers.back().accepted = true;
        states.emplace_back();
        pos.do_move(m, states.back(), nullptr);
        balance.push_back(material_balance(pos, us));
    }
    int score = 0;
    for (const auto& o : offers) {
        // An objectively acceptable offer still matters when best defence
        // declines it. Keep its bonus below an accepted substantial sacrifice.
        if (o.seeNegative)
            score = std::max(score, 100 * 100 / (o.ply + 2));
        // Require an actual material investment surviving our next move.
        // Normal trades followed by recaptures therefore earn no bonus.
        if (o.ply + 2 < int(balance.size())) {
            int loss = std::min(o.balance - balance[o.ply + 1], o.balance - balance[o.ply + 2]);
            if (loss >= 100 && o.accepted)
                score = std::max(score, std::min(loss, 900) * 100 / (o.ply + 2));
        }
    }
    return score;
}

Move select_brilliant(const Position& root, const Search::RootMoves& moves, int minDepth,
                      int maxLoss, int horizon) {
    if (moves.empty()) return Move::none();
    const auto& best = moves.front();
    if (best.pv.empty()) return Move::none();
    if (best.is_inexact() || is_decisive(best.score) || best.verifiedDepth < minDepth)
        return best.pv[0];
    const int bestCp = UCIEngine::to_cp(best.score, root);
    Move chosen = best.pv[0];
    int bonus = sacrifice_score(root, best, horizon);
    for (const auto& rm : moves) {
        if (rm.is_inexact() || is_decisive(rm.score) || rm.verifiedDepth < minDepth
            || rm.tbRank != best.tbRank || rm.pv.empty()) continue;
        int cp = UCIEngine::to_cp(rm.score, root);
        if (cp > bestCp || bestCp - cp > maxLoss || (bestCp >= 0 && cp < 0)) continue;
        int candidate = sacrifice_score(root, rm, horizon);
        if (candidate > bonus) { bonus = candidate; chosen = rm.pv[0]; }
    }
    return chosen;
}

std::string candidates_json(const Position& pos, const Search::RootMoves& moves, int depth) {
    std::ostringstream out;
    out << "{\"fen\":" << json_string(pos.fen()) << ",\"key\":" << json_string(position_key(pos))
        << ",\"depth\":" << depth << ",\"repeated\":" << (pos.has_repeated() ? "true" : "false")
        << ",\"legal_count\":" << MoveList<LEGAL>(pos).size() << ",\"moves\":[";
    bool first = true;
    for (const auto& rm : moves) {
        if (rm.pv.empty() || rm.score == -VALUE_INFINITE) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"move\":" << json_string(UCIEngine::move(rm.pv[0], pos.is_chess960()))
            << ",\"score\":" << json_string(UCIEngine::format_score({rm.score, pos}))
            << ",\"exact\":" << (rm.is_inexact() ? "false" : "true")
            << ",\"depth\":" << rm.verifiedDepth;
        if (!is_decisive(rm.score)) out << ",\"cp\":" << UCIEngine::to_cp(rm.score, pos);
        out << ",\"sacrifice\":" << sacrifice_score(pos, rm, 16) << '}';
    }
    return out.str() + "]}";
}

}  // namespace Stockfish::Alien
