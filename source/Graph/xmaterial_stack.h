#ifndef XMATERIAL_STACK_H
#define XMATERIAL_STACK_H
#pragma once

#include "xmaterial_graph_node_guid.h"
#include <unordered_map>

namespace xmaterial_graph
{
    struct graph;

    struct section
    {
        enum class kind  : std::uint8_t { USER, OUTPUT };
        enum class stage : std::uint8_t { FRAGMENT, VERTEX };       // VERTEX reserved: never created or shown in v1

        inline static constexpr auto kind_enum_v = std::array
        { xproperty::settings::enum_item{ "USER",       kind::USER      }
        , xproperty::settings::enum_item{ "OUTPUT",     kind::OUTPUT    }
        };

        inline static constexpr auto stage_enum_v = std::array
        { xproperty::settings::enum_item{ "FRAGMENT",   stage::FRAGMENT }
        , xproperty::settings::enum_item{ "VERTEX",     stage::VERTEX   }
        };

        section_guid            m_Guid       = {};
        std::string             m_Name       = {};                  // unique among sections
        kind                    m_Kind       = kind::USER;
        stage                   m_Stage      = stage::FRAGMENT;     // meaningful for OUTPUT only in v1
        bool                    m_bCollapsed = false;
        std::vector<node_guid>  m_Steps      = {};                  // ordered; the only place order lives

        XPROPERTY_DEF
        ( "section", section
        , obj_member<"Guid",       &section::m_Guid,       member_flags<flags::DONT_SHOW>>
        , obj_member<"Name",       &section::m_Name>
        , obj_member<"Kind",       &section::m_Kind,       member_enum_span<kind_enum_v>,  member_flags<flags::DONT_SHOW>>
        , obj_member<"Stage",      &section::m_Stage,      member_enum_span<stage_enum_v>, member_flags<flags::DONT_SHOW>>
        , obj_member<"bCollapsed", &section::m_bCollapsed, member_flags<flags::DONT_SHOW>>
        , obj_member<"Steps",      &section::m_Steps,      member_flags<flags::DONT_SHOW>>
        )
    };
    XPROPERTY_REG(section)

    struct stack
    {
        std::vector<section> m_Sections;       // ordered; exactly one OUTPUT section, always last

        XPROPERTY_DEF( "stack", stack, obj_member<"Sections", &stack::m_Sections> )

        // Derived, rebuilt by Reindex() after every change (never saved)
        std::unordered_map<node_guid, std::pair<int,int>>   m_Where;    // node -> (section index, step index)
        std::unordered_map<node_guid, int>                  m_Global;   // node -> global order index
        void Reindex        (void) noexcept;
        bool IsOrderValid   (const graph& G, std::vector<connection_guid>* pViolations = nullptr) const noexcept;
    };
    XPROPERTY_REG(stack)
}

#endif
