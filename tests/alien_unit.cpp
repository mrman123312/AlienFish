// AlienFish policy, persistence, and sacrifice detection tests. GPL-3.0-or-later.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "alien.h"
#include "attacks.h"
#include "uci.h"

using namespace Stockfish;
namespace fs = std::filesystem;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)

static Search::RootMove line(Position& root, std::initializer_list<const char*> moves, int cp, int depth=14) {
    Position p;
    std::deque<StateInfo> states(1);
    CHECK(!p.set(root.fen(), root.is_chess960(), &states.back()));
    Search::RootMove rm(Move::none());
    rm.pv.clear();
    for (const auto* text : moves) {
        Move m = UCIEngine::to_move(p, text);
        CHECK(m != Move::none());
        rm.pv.push_back(m);
        states.emplace_back();
        p.do_move(m, states.back());
    }
    int v = 0;
    while (UCIEngine::to_cp(v, root) < std::abs(cp)) ++v;
    rm.score = rm.uciScore = cp < 0 ? -v : v;
    rm.verifiedDepth = depth;
    return rm;
}

int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        Attacks::init();
        Position::init();
        const fs::path dir(argv[1]);
        fs::create_directories(dir);
        Position root, changed, greek, trade;
        StateInfo a, b, c, d;
        CHECK(!root.set(StartFEN, false, &a));
        CHECK(!changed.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 99", false, &b));
        CHECK(Alien::position_key(root) == Alien::position_key(changed));
        CHECK(!changed.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 40 1", false, &b));
        CHECK(Alien::position_key(root) != Alien::position_key(changed));
        CHECK(!changed.set(StartFEN, true, &b));
        CHECK(Alien::position_key(root) != Alien::position_key(changed));
        CHECK(Alien::gambit_move(changed).empty());
        CHECK(Alien::gambit_move(root) == "e2e4");

        auto strong = line(root, {"e2e4"}, 100);
        auto near = line(root, {"d2d4"}, 90);
        auto bad = line(root, {"a2a3"}, 40);
        Alien::Legacy bank;
        const auto file = dir / "test.afl";
        CHECK(bank.configure(file, "official-network", {12, 0, 20, 10}));
        bank.observe(root, {strong, near, bad}, 10000, true);
        CHECK(bank.size() == 2);
        CHECK(bank.flush().empty());
        CHECK(fs::exists(file));
        Alien::Legacy reload;
        CHECK(reload.configure(file, "official-network", {12, 0, 20, 10}));
        CHECK(reload.hints(root).size() == 2);
        CHECK(reload.hints(changed).empty());
        CHECK(reload.compact().empty());
        CHECK(!reload.configure(file, "different-network", {12, 0, 20, 10}));
        CHECK(reload.size() == 0);
        CHECK(bank.configure(file, "official-network", {12, 0, 20, 10}));
        strong.verifiedDepth = 16;
        bank.observe(root, {strong}, 20000, true);
        fs::create_directory(fs::path(file.string() + ".lock"));
        CHECK(!bank.flush().empty());
        CHECK(!bank.configure(dir / "other.afl", "different-network", {12, 0, 20, 10}));
        fs::remove(fs::path(file.string() + ".lock"));
        CHECK(bank.flush().empty());
        { std::ofstream out(file, std::ios::app); out << "broken\nP torn record"; }
        Alien::Legacy damaged;
        CHECK(damaged.configure(file, "official-network", {12, 0, 20, 10}));
        CHECK(damaged.size() == 2);
        strong.verifiedDepth = 18;
        damaged.observe(root, {strong}, 30000, true);
        CHECK(damaged.flush().empty());
        Alien::Legacy recovered;
        CHECK(recovered.configure(file, "official-network", {12, 0, 20, 10}));
        CHECK(recovered.size() == 2);
        CHECK(recovered.status().find("\"rejected\":2") != std::string::npos);
        auto negative = line(root, {"e2e4"}, -30, 20);
        recovered.observe(root, {negative}, 40000, true);
        CHECK(recovered.size() == 1);  // Deeper refutation removes old advice
        CHECK(recovered.flush().empty());
        recovered.observe(root, {strong}, 50000, true);
        CHECK(recovered.size() == 1);  // Shallower optimism cannot undo a refutation
        CHECK(recovered.compact().empty());
        Alien::Legacy refuted;
        CHECK(refuted.configure(file, "official-network", {12, 0, 20, 10}));
        CHECK(refuted.size() == 1);
        CHECK(refuted.status().find("\"refutations\":1") != std::string::npos);
        refuted.observe(root, {strong}, 60000, true);
        CHECK(refuted.size() == 1);  // Includes reload after compaction
        strong.verifiedDepth = 22;
        refuted.observe(root, {strong}, 70000, true);
        CHECK(refuted.size() == 2);  // Stronger new evidence may overturn the refutation
        CHECK(refuted.flush().empty());
        CHECK(refuted.configure(file, "official-network", {12, 0, 20, 1}));
        CHECK(refuted.size() == 1);  // Lowered capacities are enforced on reconfigure
        Alien::Legacy guarded;
        CHECK(guarded.configure(dir / "gates.afl", "official-network", {12, 0, 20, 1}));
        guarded.observe(root, {strong, near}, 10000, false);
        CHECK(guarded.size() == 0);  // Restricted roots cannot establish best-vs-alternative loss
        auto shallow = strong;
        shallow.verifiedDepth = 5;
        guarded.observe(root, {shallow}, 10000, true);
        CHECK(guarded.size() == 0);
        auto bound = strong;
        bound.inexactLower = true;
        guarded.observe(root, {bound}, 10000, true);
        CHECK(guarded.size() == 0);
        guarded.observe(root, {strong, near}, 10000, true, {"d2d4"});
        CHECK(guarded.size() == 1);
        CHECK(guarded.hints(root)[0] == "d2d4");

        CHECK(!greek.set("5rk1/5ppp/8/8/8/3B1N2/8/3Q2K1 w - - 0 1", false, &c));
        auto quiet = line(greek, {"g1h1", "g8h8"}, 100);
        auto sacrifice = line(greek, {"d3h7", "g8h7", "f3g5"}, 90);
        auto setup = line(greek, {"d1e2", "f8e8", "d3h7", "g8h7", "f3g5"}, 90);
        CHECK(Alien::sacrifice_score(greek, sacrifice, 16) > 0);
        CHECK(Alien::sacrifice_score(greek, setup, 16) > 0);
        CHECK(Alien::select_brilliant(greek, {quiet, sacrifice}, 12, 15, 16) == sacrifice.pv[0]);
        CHECK(Alien::select_brilliant(greek, {quiet, sacrifice}, 12, 0, 16) == quiet.pv[0]);
        CHECK(Alien::select_brilliant(greek, {quiet, sacrifice}, 20, 15, 16) == quiet.pv[0]);
        auto mate = quiet;
        mate.score = mate.uciScore = mate_in(5);
        CHECK(Alien::select_brilliant(greek, {mate, sacrifice}, 12, 200, 16) == mate.pv[0]);
        CHECK(!trade.set("3rk3/8/8/8/8/8/8/3R3K w - - 0 1", false, &d));
        auto exchange = line(trade, {"d1d8", "e8d8", "h1h2"}, 0);
        CHECK(Alien::sacrifice_score(trade, exchange, 16) == 0);
        Position indirect;
        StateInfo e;
        CHECK(!indirect.set("7k/8/8/5b2/8/2N5/4B3/1Q4K1 w - - 0 1", false, &e));
        auto queenOffer = line(indirect, {"c3d5", "f5b1", "e2d3"}, 100);
        CHECK(Alien::sacrifice_score(indirect, queenOffer, 16) > 0);
        std::cout << "AlienFish unit checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
