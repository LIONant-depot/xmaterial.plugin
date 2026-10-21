// Standalone self-check of the order functions (not in CMake): see the recipe in stack_canvas.md task 12.2
#define XMATERIAL_STACK_ADAPTER_CORE_ONLY
#include "xmaterial_stack_adapter.h"
#include <cstdio>
#include <random>
using namespace xmaterial_graph::stack_order;

static bool Valid(const sections& S, const edges& E)
{
    auto W = Locate(S, (int)Flatten(S).size());
    for (auto [p, c] : E) if (W.m_Global[p] >= W.m_Global[c]) return false;
    return true;
}
static bool SameSet(const sections& S, int N) { auto F = Flatten(S); std::sort(F.begin(), F.end()); for (int i = 0; i < N; ++i) if ((int)F.size() != N || F[i] != i) return false; return true; }

int main()
{
    // sort: stable on valid input, fixes a violation, reports a cycle without losing nodes
    { std::vector<int> O{0,1,2,3}; assert(StableTopoSort(O, {{0,1},{1,2}}) && O == std::vector<int>({0,1,2,3})); }
    { std::vector<int> O{2,1,0,3}; assert(StableTopoSort(O, {{0,1},{1,2}}) && O == std::vector<int>({0,1,2,3})); }
    { std::vector<int> O{0,1,2}; assert(!StableTopoSort(O, {{0,1},{1,0}}) && O.size() == 3); }

    // 0->2, 1->2, 2->3 ; sections {0,1} {2} {3 output}
    sections S{{0,1},{2},{3}}; edges E{{0,2},{1,2},{2,3}}; std::vector<char> Out{0,0,1};
    assert(ValidRange(S, E, 2, 0) == std::make_pair(2,2));
    assert(ValidRange(S, E, 2, 1) == std::make_pair(0,0));
    assert(ValidRange(S, E, 2, 2) == std::make_pair(0,0));
    { auto R = ValidRange(S, E, 1, 2); assert(R.first > R.second); }       // producer cannot go after its consumer's section
    assert(DefaultSlot(S, Out, E, 2) == std::make_pair(0,2));              // right after latest producer (1)
    assert(DefaultSlot(S, Out, {{2,3}}, 2) == std::make_pair(2,0));          // no producer: right before its earliest consumer (3)
    assert(DefaultSlot({{0},{2}}, Out = {0,1}, {}, 1) == std::make_pair(0,1));   // orphan: end of last USER section
    assert(DefaultSlot({{1}}, Out = {1}, {}, 0).first == -1);              // no user section

    // move 2 up to section 0 slot 0 (before its producers): dependents drags 0,1 after? no: producers dragged before it
    { auto R = ResolveMove(S, E, {2}, 0, 0);
      assert(Valid(R.m_Dependents, E) && Valid(R.m_Nearest, E));
      assert(R.m_Nearest[0] == std::vector<int>({0,1,2}));                 // clamped just after the latest producer
      assert(R.m_Dependents[0] == std::vector<int>({0,1,2})); }            // producers dragged to right before it, relative order kept
    // move 0 down past its consumer 2 (section 2 slot 0): consumer 2 dragged after it, and 3 stays valid
    { auto R = ResolveMove(S, E, {0}, 1, 1);
      assert(Valid(R.m_Dependents, E) && Valid(R.m_Nearest, E));
      assert(R.m_Dependents[1] == std::vector<int>({0,2})); }                 // consumer 2 dragged to right after the block

    // fuzz: random DAGs, random valid orders, random moves: both results valid and nothing lost
    std::mt19937 Rng(7);
    for (int it = 0; it < 20000; ++it)
    {
        int N = 2 + Rng() % 9; std::vector<int> Perm(N); for (int i = 0; i < N; ++i) Perm[i] = i; std::shuffle(Perm.begin(), Perm.end(), Rng);
        edges E2; for (int a = 0; a < N; ++a) for (int b = a + 1; b < N; ++b) if (Rng() % 4 == 0) E2.push_back({Perm[a], Perm[b]});   // acyclic by construction
        std::vector<int> O(N); for (int i = 0; i < N; ++i) O[i] = i; std::shuffle(O.begin(), O.end(), Rng);
        assert(StableTopoSort(O, E2));
        {   auto Copy = O; assert(StableTopoSort(Copy, E2) && Copy == O); }                // stable: already valid stays
        int nSec = 1 + Rng() % 4; sections S2(nSec); for (int n : O) S2[Rng() % nSec].push_back(n);
        // cut into contiguous sections instead (a random split keeps validity)
        S2.assign(nSec, {}); for (int i = 0, s = 0; i < N; ++i) { if (s + 1 < nSec && Rng() % 3 == 0) ++s; S2[s].push_back(O[i]); }
        assert(Valid(S2, E2));
        // block of one or two nodes, forming a valid sub-order
        std::vector<int> B{ (int)(Rng() % N) }; if (N > 3 && Rng() % 2) { int x = (int)(Rng() % N); if (x != B[0]) B.push_back(x); }
        { auto Fl = Flatten(S2); std::sort(B.begin(), B.end(), [&](int a, int b){ return std::find(Fl.begin(), Fl.end(), a) < std::find(Fl.begin(), Fl.end(), b); }); }
        // the block must be convex: no path between two of its members (direct or through outside nodes) -- the caller's precondition
        bool bInner = false;
        { std::vector<std::vector<char>> Reach(N, std::vector<char>(N, 0)); for (auto [p, c] : E2) Reach[p][c] = 1;
          for (int k = 0; k < N; ++k) for (int i = 0; i < N; ++i) for (int j = 0; j < N; ++j) if (Reach[i][k] && Reach[k][j]) Reach[i][j] = 1;
          for (int a : B) for (int b : B) if (Reach[a][b]) bInner = true; }
        if (bInner) continue;
        int Sec = Rng() % nSec; int Slot = Rng() % (S2[Sec].size() + 1);
        auto R = ResolveMove(S2, E2, B, Sec, Slot);
        assert(SameSet(R.m_Dependents, N) && SameSet(R.m_Nearest, N) && (int)R.m_Dependents.size() == nSec);
        assert(Valid(R.m_Dependents, E2)); assert(Valid(R.m_Nearest, E2));
        // ValidRange agrees with brute force for a single node
        int Node = (int)(Rng() % N); int s = Rng() % nSec;
        auto VR = ValidRange(S2, E2, Node, s);
        for (int k = 0; k <= (int)SizeWithout(S2, s, Node); ++k)
        {
            sections T = S2; for (auto& q : T) q.erase(std::remove(q.begin(), q.end(), Node), q.end());
            T[s].insert(T[s].begin() + k, Node);
            assert(Valid(T, E2) == (k >= VR.first && k <= VR.second));
        }
    }
    std::puts("stack adapter self-check: OK");
    return 0;
}
