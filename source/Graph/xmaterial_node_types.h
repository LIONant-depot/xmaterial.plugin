#ifndef XMATERIAL_NODE_TYPES_H
#define XMATERIAL_NODE_TYPES_H
#pragma once

// The material graph's pin types and node types as data (xLION_Docs material_pipeline_for_render_graph section 9, Tomas 2026-11-09):
// <plugin>/NodeTypes/Types.txt (the pin types) and one <Name>.matn.txt per node type (Tomas: the double extension says it is a material node) (its name, category, description, pins, params) with its
// GLSL template in the file its Code column names (beside it) (the template exactly: $var, $input[n], [TOP]<!..!> ...). Each has an Id (a guid: materials refer to
// it - never changed; a new type takes a new one); pins name their type as Types.txt does ("Vector4"). The structural nodes (Group Input/Output, Group,
// Comment) stay in C++ (graph::CreateGraph). A file that does not load is said (its path and why) and skipped; the others load.
#include "xmaterial_graph.h"
#include "dependencies/xtextfile/source/xtextfile.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace xmaterial_graph::node_types
{
    inline const char* ParamTypeName(node_param::type T) noexcept
    {
        switch (T)
        {
        case node_param::type::FLOAT:             return "float";
        case node_param::type::INT:               return "int";
        case node_param::type::STRING:            return "string";
        case node_param::type::FILE:              return "file";
        case node_param::type::TEXTURE_RESOURCE:  return "texture";
        case node_param::type::SUBGRAPH_RESOURCE: return "subgraph";
        default:                                  return "none";
        }
    }
    inline node_param::type ParamTypeOf(std::string_view S) noexcept
    {
        for (auto T : { node_param::type::FLOAT, node_param::type::INT, node_param::type::STRING, node_param::type::FILE, node_param::type::TEXTURE_RESOURCE, node_param::type::SUBGRAPH_RESOURCE })
            if (S == ParamTypeName(T)) return T;
        return node_param::type::NONE;
    }

    // One node type's file: the rows of its tables (reading or writing the same way)
    struct file
    {
        struct pin_row   { std::string m_Name, m_Type, m_Expression; int m_Param = -1; };
        struct param_row { std::string m_Name, m_Type, m_Value; bool m_bCanExpose = false; float m_Min = 0, m_Max = 0; };      // (Min == Max: no range)
        std::uint64_t           m_Id = 0;                                                // the node type's guid (materials refer to it)
        std::string             m_Name, m_Category, m_Code, m_Description;
        std::vector<pin_row>    m_Inputs, m_Outputs;
        std::vector<param_row>  m_Params;

        xerr Serialize(xtextfile::stream& F) noexcept
        {
            xerr Err;
            if ((Err = F.Record("NodeType", [&](xerr& E)
                { false || (E = F.Field("Id", m_Id)) || (E = F.Field("Name", m_Name)) || (E = F.Field("Category", m_Category)) || (E = F.Field("Code", m_Code)) || (E = F.Field("Description", m_Description)); }))) return Err;
            const auto PinRow = [&](std::vector<pin_row>& Rows, std::size_t i, xerr& E) { false || (E = F.Field("Name", Rows[i].m_Name)) || (E = F.Field("Type", Rows[i].m_Type)) || (E = F.Field("Param", Rows[i].m_Param)) || (E = F.Field("Expression", Rows[i].m_Expression)); };
            if ((Err = F.Record("Inputs"
                , [&](std::size_t& Count, xerr&) { if (F.isReading()) m_Inputs.resize(Count); else Count = m_Inputs.size(); }
                , [&](std::size_t i, xerr& E) { PinRow(m_Inputs, i, E); }))) return Err;
            if ((Err = F.Record("Outputs"
                , [&](std::size_t& Count, xerr&) { if (F.isReading()) m_Outputs.resize(Count); else Count = m_Outputs.size(); }
                , [&](std::size_t i, xerr& E) { PinRow(m_Outputs, i, E); }))) return Err;
            if ((Err = F.Record("Params"
                , [&](std::size_t& Count, xerr&) { if (F.isReading()) m_Params.resize(Count); else Count = m_Params.size(); }
                , [&](std::size_t i, xerr& E) { false || (E = F.Field("Name", m_Params[i].m_Name)) || (E = F.Field("Type", m_Params[i].m_Type)) || (E = F.Field("Value", m_Params[i].m_Value)) || (E = F.Field("CanExpose", m_Params[i].m_bCanExpose)) || (E = F.Field("Min", m_Params[i].m_Min)) || (E = F.Field("Max", m_Params[i].m_Max)); }))) return Err;
            return {};
        }
    };

    // The pin types' file: a row each
    struct types_file
    {
        struct row { std::uint64_t m_Id = 0, m_SubType = 0; std::string m_Name, m_Code, m_Sub; int m_R = 0, m_G = 0, m_B = 0, m_A = 255; };
        std::vector<row> m_Rows;
        xerr Serialize(xtextfile::stream& F) noexcept
        {
            return F.Record("Types"
                , [&](std::size_t& Count, xerr&) { if (F.isReading()) m_Rows.resize(Count); else Count = m_Rows.size(); }
                , [&](std::size_t i, xerr& E)
                {
                    auto& R = m_Rows[i];
                    false || (E = F.Field("Id", R.m_Id)) || (E = F.Field("Name", R.m_Name)) || (E = F.Field("Code", R.m_Code))
                          || (E = F.Field("R", R.m_R)) || (E = F.Field("G", R.m_G)) || (E = F.Field("B", R.m_B)) || (E = F.Field("A", R.m_A))
                          || (E = F.Field("Sub", R.m_Sub)) || (E = F.Field("SubType", R.m_SubType));
                });
        }
    };

    inline std::string FileName(std::string_view Name) noexcept      // "Vec4 Multiply" -> "Vec4_Multiply" (letters, digits, _ and - only)
    {
        std::string S;
        for (char c : Name) S += (std::isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '_';
        while (S.find("__") != std::string::npos) S.replace(S.find("__"), 2, "_");
        while (!S.empty() && S.back() == '_') S.pop_back();
        return S;
    }

    inline std::string ReadAll(const std::filesystem::path& P, bool& bOk) noexcept      // (a template exactly, CR dropped: git may add them)
    {
        std::ifstream In(P, std::ios::binary);
        bOk = In.is_open();
        std::stringstream S; S << In.rdbuf();
        std::string T = S.str();
        std::erase(T, '\r');
        return T;
    }

    //---------------------------------------------------------------------------------------------------------------------------------
    // Load: the folder's types and node types into G (after its structural nodes). Problems go to Report (a line each: the file and why)
    struct loaded { std::vector<std::pair<std::uint64_t, sType>> m_Types; std::vector<std::pair<std::uint64_t, node>> m_Nodes; std::string m_Problems; };

    inline loaded Read(const std::filesystem::path& Folder) noexcept
    {
        loaded L;
        const auto Problem = [&](const std::filesystem::path& P, std::string_view Why) { L.m_Problems += std::format("{}: {}\n", P.string(), Why); };
        types_file T;
        {
            const auto P = Folder / "Types.txt";
            xtextfile::stream F;
            if (auto Err = F.Open(true, P.wstring(), xtextfile::file_type::TEXT); Err) { Problem(P, "the pin types file does not open (no node types without it)"); return L; }
            if (auto Err = T.Serialize(F); Err) { Problem(P, std::format("not read: {}", Err.getMessage())); return L; }
        }
        for (auto& R : T.m_Rows)
        {
            sType S;
            S.m_GUID = type_guid{ R.m_Id }; S.m_Name = R.m_Name; S.m_CodeString = R.m_Code;
            S.m_Color = FromRGB(R.m_R, R.m_G, R.m_B, R.m_A);
            for (std::size_t b = 0; b < R.m_Sub.size(); )
            {
                std::size_t e = R.m_Sub.find(' ', b); if (e == std::string::npos) e = R.m_Sub.size();
                if (e > b) { auto& V = S.m_Sub.emplace_back(); V.m_TypeGUID = type_guid{ R.m_SubType }; V.m_Name = R.m_Sub.substr(b, e - b); }
                b = e + 1;
            }
            L.m_Types.emplace_back(S.m_GUID.m_Value, std::move(S));
        }
        // A pin's type by its name in Types.txt (readable: "Vector4"); 0: no such type
        const auto TypeOf = [&](std::string_view Name) -> std::uint64_t { for (auto& [Id, S] : L.m_Types) if (S.m_Name == Name) return Id; return 0; };
        std::vector<std::filesystem::path> Files;
        for (auto& E : std::filesystem::directory_iterator(Folder)) if (E.path().filename().string().ends_with(".matn.txt")) Files.push_back(E.path());      // (a material node: <Name>.matn.txt)
        std::ranges::sort(Files);
        for (auto& P : Files)
        {
            file D;
            xtextfile::stream F;
            if (auto Err = F.Open(true, P.wstring(), xtextfile::file_type::TEXT); Err) { Problem(P, "does not open"); continue; }
            if (auto Err = D.Serialize(F); Err) { Problem(P, std::format("not read: {}", Err.getMessage())); continue; }
            if (D.m_Id == 0 || D.m_Name.empty() || D.m_Code.empty()) { Problem(P, "its NodeType row needs an Id, a Name and a Code file"); continue; }
            bool bOk = true;
            node N;
            N.m_Name = D.m_Name; N.m_Category = D.m_Category; N.m_Description = D.m_Description;
            const auto Template = std::filesystem::path(P).parent_path() / D.m_Code;                              // (Code: the template, beside the file)
            N.m_Code = ReadAll(Template, bOk);
            if (!bOk) { Problem(P, std::format("its template {} is missing", Template.filename().string())); continue; }
            std::string Why;
            for (auto& R : D.m_Inputs)  { if (!TypeOf(R.m_Type)) Why = std::format("input '{}' has the unknown type '{}' (Types.txt names them)", R.m_Name, R.m_Type);  auto& I = N.m_InputPins.emplace_back();  I.m_Name = R.m_Name; I.m_TypeGUID = type_guid{ TypeOf(R.m_Type) }; I.m_ParamIndex = R.m_Param; I.m_DefaultExpr = R.m_Expression; }
            for (auto& R : D.m_Outputs) { if (!TypeOf(R.m_Type)) Why = std::format("output '{}' has the unknown type '{}' (Types.txt names them)", R.m_Name, R.m_Type); auto& O = N.m_OutputPins.emplace_back(); O.m_Name = R.m_Name; O.m_TypeGUID = type_guid{ TypeOf(R.m_Type) }; O.m_ParamIndex = R.m_Param; O.m_DefaultExpr = R.m_Expression; }
            for (auto& R : D.m_Params)
            {
                const auto T2 = ParamTypeOf(R.m_Type);
                if (T2 == node_param::type::NONE) { Why = std::format("param '{}' has the unknown type '{}' (float, int, string, file, texture, subgraph)", R.m_Name, R.m_Type); break; }
                switch (T2)
                {
                case node_param::type::FLOAT:  N.m_Params.emplace_back(R.m_Name, static_cast<float>(std::atof(R.m_Value.c_str()))); break;
                case node_param::type::INT:    N.m_Params.emplace_back(R.m_Name, std::atoi(R.m_Value.c_str())); break;
                case node_param::type::STRING: N.m_Params.emplace_back(R.m_Name, std::string(R.m_Value)); break;
                case node_param::type::FILE:   N.m_Params.emplace_back(R.m_Name, xstrtool::To(R.m_Value)); break;
                default:                       N.m_Params.emplace_back(R.m_Name, T2, xresource::full_guid{}); break;
                }
                N.m_Params.back().m_bCanExpose = R.m_bCanExpose; N.m_Params.back().m_Min = R.m_Min; N.m_Params.back().m_Max = R.m_Max;
            }
            for (auto& I : N.m_InputPins) if (I.m_ParamIndex >= static_cast<int>(N.m_Params.size())) Why = std::format("input '{}' reads param {} of {}", I.m_Name, I.m_ParamIndex, N.m_Params.size());
            if (!Why.empty()) { Problem(P, Why); continue; }
            if (std::ranges::any_of(L.m_Nodes, [&](auto& E) { return E.first == D.m_Id; })) { Problem(P, std::format("its Id #{:016X} is another node type's too", D.m_Id)); continue; }
            L.m_Nodes.emplace_back(D.m_Id, std::move(N));
        }
        return L;
    }

    // Once a process per folder (CreateGraph runs for every graph: each document, compile, sub-graph, step preview)
    inline const loaded& Cached(const std::filesystem::path& Folder) noexcept
    {
        static std::mutex s_Lock;
        static std::unordered_map<std::wstring, loaded> s_Cache;
        std::scoped_lock Lk(s_Lock);
        auto [It, bNew] = s_Cache.try_emplace(Folder.wstring());
        if (bNew)
        {
            It->second = Read(Folder);
            if (!It->second.m_Problems.empty()) { std::fprintf(stderr, "Material node types (%ls):\n%s", Folder.c_str(), It->second.m_Problems.c_str()); std::fflush(stderr); }
        }
        return It->second;
    }
}

#endif
