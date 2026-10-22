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

    // Where the person put a step on the canvas (a step's guid, or a collapsed section's): its depth column and its top. The layout never
    // overrides it (it only clamps the column into the ones the step may sit in); "Clean graph" clears them all.
    struct place
    {
        node_guid       m_Step   = {};      // a collapsed section: its section guid's value
        int             m_Column = 0;
        float           m_Y      = 0;

        XPROPERTY_DEF
        ( "place", place
        , obj_member<"Step",   &place::m_Step>
        , obj_member<"Column", &place::m_Column>
        , obj_member<"Y",      &place::m_Y>
        )
    };
    XPROPERTY_REG(place)

    struct stack
    {
        std::vector<section> m_Sections;       // ordered; exactly one OUTPUT section, always last
        std::vector<place>   m_Places;         // steps the person placed by hand (any order, one per step)

        XPROPERTY_DEF( "stack", stack, obj_member<"Sections", &stack::m_Sections>, obj_member<"Places", &stack::m_Places> )

        // Derived, rebuilt by Reindex() after every change (never saved)
        std::unordered_map<node_guid, std::pair<int,int>>   m_Where;    // node -> (section index, step index)
        std::unordered_map<node_guid, int>                  m_Global;   // node -> global order index
        void Reindex        (void) noexcept;
        bool IsOrderValid   (const graph& G, std::vector<connection_guid>* pViolations = nullptr) const noexcept;
    };
    XPROPERTY_REG(stack)
}

#endif
