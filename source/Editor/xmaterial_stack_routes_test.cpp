// Standalone self-check of the stack layout and router (not in CMake). Build from D:\LIONant\xLION with
//   cl /nologo /std:c++latest /EHsc /Zc:preprocessor /I. /Idependencies/xtextfile /Idependencies/xerr <this file>
// Usage: test [fuzz cases] [svg path]. Runs SelfTest, then routes the example materials (v1 files: read only, migrated in memory the way
// the adapter does, minus comment rectangles) and writes an SVG of the largest one.
#define XMATERIAL_STACK_ROUTES_CORE_ONLY
#define XMATERIAL_STACK_ADAPTER_CORE_ONLY
#include "xmaterial_stack_routes.h"
#include "../Graph/xmaterial_stack_adapter.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <set>
#include <cstring>
#include <numeric>

using namespace xmaterial_editor;
namespace sr = stack_routes;
namespace sl = stack_layout;

struct mnode { std::string m_Name; float x = 0, y = 0; std::vector<std::string> m_InNames; std::vector<std::uint64_t> m_In, m_Out; std::vector<std::vector<std::uint64_t>> m_Sub; std::vector<std::string> m_OutNames; };
struct material { std::string m_Path; std::vector<mnode> m_Nodes; std::vector<std::pair<std::uint64_t, std::uint64_t>> m_Links; };   // (output pin, input pin)

static std::uint64_t Hex(const std::string& L) { auto p = L.find('#'); return p == std::string::npos ? 0 : std::stoull(L.substr(p + 1), nullptr, 16); }
static std::string   Str(const std::string& L) { auto a = L.find('"', L.find(';')); auto b = L.find('"', a + 1); return L.substr(a + 1, b - a - 1); }
static int           Index(const std::string& L, const char* Key) { auto p = L.find(Key); return p == std::string::npos ? -1 : std::stoi(L.substr(p + std::strlen(Key))); }
template<typename T> static T& At(std::vector<T>& V, int i) { if ((int)V.size() <= i) V.resize(i + 1); return V[i]; }

static material Parse(const std::filesystem::path& P)
{
    material M; M.m_Path = P.string();
    std::ifstream F(P); std::string L; int Mode = 0;
    while (std::getline(F, L))
    {
        if (L.starts_with("[ Node ]"))       { Mode = 1; M.m_Nodes.emplace_back(); continue; }
        if (L.starts_with("[ Connection ]")) { Mode = 2; M.m_Links.emplace_back(); continue; }
        if (Mode == 1)
        {
            auto& N = M.m_Nodes.back();
            if      (L.find("\"node/Name\"") != std::string::npos)         N.m_Name = Str(L);
            else if (L.find("\"node/Position/X\"") != std::string::npos)   N.x = std::stof(L.substr(L.find("f32") + 3));
            else if (L.find("\"node/Position/Y\"") != std::string::npos)   N.y = std::stof(L.substr(L.find("f32") + 3));
            else if (L.find("\"node/Input Pins[G:") != std::string::npos && L.find("/pin/PinGUID/Value") != std::string::npos) At(N.m_In, Index(L, "Input Pins[G:")) = Hex(L);
            else if (L.find("\"node/Input Pins[G:") != std::string::npos && L.find("/pin/var/Name") != std::string::npos)      At(N.m_InNames, Index(L, "Input Pins[G:")) = Str(L);
            else if (L.find("\"node/Output Pins[G:") != std::string::npos && L.find("SubElements[G:") != std::string::npos && L.find("/PinGUID/Value") != std::string::npos)
                At(At(N.m_Sub, Index(L, "Output Pins[G:")), Index(L, "SubElements[G:")) = Hex(L);
            else if (L.find("\"node/Output Pins[G:") != std::string::npos && L.find("/pin/PinGUID/Value") != std::string::npos) At(N.m_Out, Index(L, "Output Pins[G:")) = Hex(L);
            else if (L.find("\"node/Output Pins[G:") != std::string::npos && L.find("/pin/var/Name") != std::string::npos)      At(N.m_OutNames, Index(L, "Output Pins[G:")) = Str(L);
        }
        else if (Mode == 2)
        {
            if      (L.find("connection/InputPinGuid") != std::string::npos)  M.m_Links.back().second = Hex(L);
            else if (L.find("connection/OutputPinGuid") != std::string::npos) M.m_Links.back().first  = Hex(L);
        }
    }
    return M;
}

struct built { sl::input m_In; std::vector<sr::edge> m_E; std::vector<std::string> m_StepNames, m_SectionNames; int m_nLinks = 0; };

// The adapter's migration minus comment rectangles: seed order by old position, groups Inputs / Steps / Output, stable topological sort;
// rows as FromGraph makes them (inputs, then used outputs, the first always; 7 px per char)
static built Migrate(const material& M)
{
    built B;
    std::vector<int> Steps;
    for (int i = 0; i < (int)M.m_Nodes.size(); ++i) if (!M.m_Nodes[i].m_In.empty() || !M.m_Nodes[i].m_Out.empty()) Steps.push_back(i);
    std::stable_sort(Steps.begin(), Steps.end(), [&](int a, int b) { auto& A = M.m_Nodes[a]; auto& Bn = M.m_Nodes[b]; return A.x != Bn.x ? A.x < Bn.x : A.y < Bn.y; });
    std::unordered_map<std::uint64_t, std::pair<int,int>> OutPin, InPin;            // pin -> (step id, output/input index)
    for (int k = 0; k < (int)Steps.size(); ++k)
    {
        auto& N = M.m_Nodes[Steps[k]];
        for (int i = 0; i < (int)N.m_In.size(); ++i) InPin[N.m_In[i]] = { k, i };
        for (int o = 0; o < (int)N.m_Out.size(); ++o) { OutPin[N.m_Out[o]] = { k, o }; if (o < (int)N.m_Sub.size()) for (auto s : N.m_Sub[o]) OutPin[s] = { k, o }; }
    }
    xmaterial_graph::stack_order::edges E;
    std::vector<std::tuple<int,int,int,int,std::uint64_t>> Links;                  // (from, out, to, in, source pin)
    for (auto [o, i] : M.m_Links)
    {
        auto O = OutPin.find(o); auto I = InPin.find(i);
        if (O == OutPin.end() || I == InPin.end() || O->second.first == I->second.first) continue;
        E.push_back({ O->second.first, I->second.first });
        Links.push_back({ O->second.first, O->second.second, I->second.first, I->second.second, o });
    }
    B.m_nLinks = (int)Links.size();
    std::vector<int> Order(Steps.size()); std::iota(Order.begin(), Order.end(), 0);
    xmaterial_graph::stack_order::StableTopoSort(Order, E);
    auto Group = [&](int k) { auto& N = M.m_Nodes[Steps[k]]; return N.m_Out.empty() ? 2 : N.m_In.empty() ? 0 : 1; };
    std::vector<int> First(3, -1);
    for (int k = 0; k < (int)Order.size(); ++k) if (First[Group(Order[k])] < 0) First[Group(Order[k])] = k;
    std::vector<int> G{ 0, 1 }; std::stable_sort(G.begin(), G.end(), [&](int a, int b) { return First[a] < First[b]; }); G.push_back(2);
    std::vector<int> Cat; for (int g : G) for (int k : Order) if (Group(k) == g) Cat.push_back(k);
    xmaterial_graph::stack_order::StableTopoSort(Cat, E);

    std::set<std::pair<int,int>> Used; for (auto& l : Links) Used.insert({ std::get<0>(l), std::get<1>(l) });
    std::vector<int> Global(Steps.size()); std::vector<std::vector<int>> OutRow(Steps.size());
    const char* Names[3] = { "Inputs", "Steps", "Output" };
    for (std::size_t k = 0; k < Cat.size(); ++k)
    {
        const int s = Cat[k], g = Group(s);
        if (B.m_In.empty() || B.m_SectionNames.back() != Names[g]) { B.m_In.push_back({ 100 + B.m_In.size(), {} }); B.m_SectionNames.push_back(Names[g]); }
        auto& N = M.m_Nodes[Steps[s]];
        float TextW = 7.0f * N.m_Name.size();
        int Row = (int)N.m_In.size();
        for (auto& n : N.m_InNames) TextW = std::max(TextW, 7.0f * n.size() + 80);
        OutRow[s].assign(N.m_Out.size(), -1);
        for (int o = 0; o < (int)N.m_Out.size(); ++o) if (o == 0 || Used.count({ s, o })) { OutRow[s][o] = Row++; if (o < (int)N.m_OutNames.size()) TextW = std::max(TextW, 7.0f * N.m_OutNames[o].size()); }
        Global[s] = (int)B.m_StepNames.size();
        B.m_StepNames.push_back(N.m_Name);
        B.m_In.back().m_Steps.push_back({ 1000 + (std::uint64_t)s, sl::StepW(TextW), std::max(1, Row) });
    }
    if (B.m_SectionNames.empty() || B.m_SectionNames.back() != "Output") { B.m_In.push_back({ 999, {} }); B.m_SectionNames.push_back("Output"); }
    for (auto& [f, o, t, i, Pin] : Links) B.m_E.push_back({ Global[f], OutRow[f][o], Global[t], i, Pin, B.m_E.size() + 1 });
    return B;
}

static void Svg(const built& B, const sr::result& R, const std::string& Path)
{
    const auto& L = R.m_Layout;
    std::ostringstream S;
    S << "<svg xmlns='http://www.w3.org/2000/svg' width='" << L.m_Width << "' height='" << L.m_Height + 20 << "' font-family='Segoe UI,Arial' font-size='11'>\n";
    S << "<rect width='100%' height='100%' fill='#1e1f22'/>\n";
    for (std::size_t s = 0; s < L.m_Sections.size(); ++s)
    {
        auto& b = L.m_Sections[s];
        S << "<rect x='" << b.m_X << "' y='" << b.m_Y << "' width='" << b.m_W << "' height='" << b.m_H << "' rx='6' fill='#26282c' stroke='#4a4d55'/>\n";
        S << "<text x='" << b.m_X + 8 << "' y='" << b.m_Y + 18 << "' fill='#9aa0aa' font-weight='bold'>" << B.m_SectionNames[s] << "</text>\n";
    }
    for (std::size_t g = 0; g < L.m_Steps.size(); ++g)
    {
        auto& b = L.m_Steps[g];
        S << "<rect x='" << b.m_X << "' y='" << b.m_Y << "' width='" << b.m_W << "' height='" << b.m_H << "' rx='4' fill='#33363c' stroke='#6b7080'/>\n";
        S << "<text x='" << b.m_X + 8 << "' y='" << b.m_Y + 17 << "' fill='#e6e6e6'>" << B.m_StepNames[g] << "</text>\n";
        for (int r = 0; r * sl::geo::ROW_H + sl::geo::HEADER_H < b.m_H - sl::geo::NODE_PAD_BOTTOM; ++r)
            S << "<line x1='" << b.m_X + 6 << "' x2='" << b.m_X + b.m_W - 6 << "' y1='" << b.m_Y + sl::geo::HEADER_H + r * sl::geo::ROW_H << "' y2='" << b.m_Y + sl::geo::HEADER_H + r * sl::geo::ROW_H << "' stroke='#3d4047'/>\n";
    }
    const char* Pal[] = { "#e5c07b", "#61afef", "#98c379", "#e06c75", "#c678dd", "#56b6c2", "#d19a66", "#abb2bf" };
    for (auto& w : R.m_Wires)
        S << "<line x1='" << w.m_X0 << "' y1='" << w.m_Y0 << "' x2='" << w.m_X1 << "' y2='" << w.m_Y1 << "' stroke='" << Pal[w.m_Source % 8] << "' stroke-width='1.6'/>\n";
    for (std::size_t i = 0; i < B.m_E.size(); ++i)                  // pin glyphs at both ends
        for (auto p : { R.m_Routes[i].m_Points.front(), R.m_Routes[i].m_Points.back() })
            S << "<circle cx='" << p.m_X << "' cy='" << p.m_Y << "' r='3.5' fill='" << Pal[R.m_RouteSource[i] % 8] << "'/>\n";
    for (auto& j : R.m_Junctions) S << "<circle cx='" << j.m_X << "' cy='" << j.m_Y << "' r='3' fill='" << Pal[j.m_Source % 8] << "'/>\n";
    S << "</svg>\n";
    std::ofstream(Path) << S.str();
}

int main(int argc, char** argv)
{
    const int Fuzz = argc > 1 ? std::atoi(argv[1]) : 2000;
    std::fputs(sr::SelfTest(Fuzz).c_str(), stdout);

    const std::filesystem::path Root = "example.lionprj/Descriptors/Material";
    built Largest; sr::result LargestR; std::size_t Most = 0;
    for (auto& F : std::filesystem::recursive_directory_iterator(Root))
    {
        if (F.path().filename() != "Descriptor.txt") continue;
        const auto B = Migrate(Parse(F.path()));
        for (float W : { 1600.0f, 800.0f })
        {
            sr::view_memory VM;
            const auto R = sr::Build(B.m_In, B.m_E, W, VM);
            sr::CheckRoutes(R, B.m_E);
            int Hw = 0, Ch = 0, MaxL = 0;
            for (int c = 0; c < R.m_Layout.m_K; ++c) { Hw += R.m_LanesL[c] + R.m_LanesR[c]; MaxL = std::max({ MaxL, R.m_LanesL[c], R.m_LanesR[c] }); for (int n : R.m_ChannelLanes[c]) Ch += n; }
            std::printf("%s @%4.0f: steps %zu routes %zu outputs %zu | columns %d | highway lanes %d (max %d) channel lanes %d | crossings %zu junctions %zu | through steps %d\n",
                F.path().parent_path().filename().string().c_str(), W, B.m_StepNames.size(), B.m_E.size(), R.m_Sources.size(), R.m_Layout.m_K, Hw, MaxL, Ch,
                R.m_Crossings.size(), R.m_Junctions.size(), R.m_StepHits);
            if (W == 1600.0f && B.m_E.size() > Most) { Most = B.m_E.size(); Largest = B; LargestR = R; }
        }
    }
    std::fputs(sr::Stats(LargestR, Largest.m_SectionNames).c_str(), stdout);
    const std::string Path = argc > 2 ? argv[2] : (std::filesystem::temp_directory_path() / "stack_routes.svg").string();
    Svg(Largest, LargestR, Path);
    std::printf("svg: %s\nstack routes self-check: OK\n", Path.c_str());
    return 0;
}
