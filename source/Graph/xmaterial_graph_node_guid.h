#ifndef XMATERIA_GRAPH_NODE_GUID_H
#define XMATERIA_GRAPH_NODE_GUID_H
#pragma once

#include "dependencies/xproperty/source/xcore/my_properties.h"
#include "dependencies/xmath/source/xmath.h"

namespace xmaterial_graph
{
    using node_guid         = xmath::strong_typing_direct_numerics_t<std::uint64_t, struct node_guid_tag>;
    using connection_guid   = xmath::strong_typing_direct_numerics_t<std::uint64_t, struct connection_guid_tag>;
    using type_guid         = xmath::strong_typing_direct_numerics_t<std::uint64_t, struct type_guid_tag>;
    using pin_guid          = xmath::strong_typing_direct_numerics_t<std::uint64_t, struct pin_guid_tag>;

    struct node_guid_friend : xmaterial_graph::node_guid
    {
        XPROPERTY_DEF
        ( "node_guid", xmaterial_graph::node_guid
        , obj_member<"Value", &xmaterial_graph::node_guid::m_Value >
        )
    };
    XPROPERTY_REG(node_guid_friend)

    struct connection_guid_friend : connection_guid
    {
        XPROPERTY_DEF
        ( "connection_guid", connection_guid
        , obj_member<"Value", &connection_guid::m_Value >
        )
    };
    XPROPERTY_REG(connection_guid_friend)

    struct pin_guid_friend : pin_guid
    {
        XPROPERTY_DEF
        ( "pin_guid", pin_guid
        , obj_member<"Value", &pin_guid::m_Value >
        )
    };
    XPROPERTY_REG(pin_guid_friend) 

    struct var
    {
        type_guid	m_TypeGUID  = {};
        std::string m_Name      = {};
        XPROPERTY_DEF
        ( "var", var
        , obj_member<"Name",    &var::m_Name,       member_flags<flags::DONT_SHOW> >
        , obj_member<"TypeGUID", +[](var& O) -> auto& { return O.m_TypeGUID.m_Value; },   member_flags<flags::DONT_SHOW> >
        )
    };
    XPROPERTY_REG(var)

    struct pin : var
    {
        pin_guid            m_PinGUID       = {};  //unique id per pin
        std::string         m_DefaultExpr   = {};
        int                 m_ParamIndex    = -1;  // -1 = no property
        XPROPERTY_DEF
        ( "pin", pin
        , obj_base<var>
        , obj_member<"PinGUID",    &pin::m_PinGUID, member_flags<flags::DONT_SHOW>, member_flags<flags::DONT_SHOW> > 
        , obj_member<"ParamIndex", &pin::m_ParamIndex,  member_flags<flags::DONT_SHOW> >
        )
    };
    XPROPERTY_REG(pin)

    struct ColorRGBA
    {
        float r, g, b, a;
        constexpr ColorRGBA(float R = 0, float G = 0, float B = 0, float A = 1.0f)
            : r(R), g(G), b(B), a(A) {}
    };

    inline ColorRGBA FromRGB(int R, int G, int B, int A = 255)
    {
        return ColorRGBA(
            R / 255.0f,
            G / 255.0f,
            B / 255.0f,
            A / 255.0f
        );
    }

    struct sType //eg float , vec3 etc , every type has a diff color for pin
    {
        //Type guid .generate a random type guid 
        type_guid               m_GUID          = {}; // unique type id
        std::string             m_Name          = {};
        std::string             m_CodeString    = {}; // e.g. "float", "vec3"
        std::vector<var>        m_Sub           = {}; // sub-components for vector types (X,Y,Z)
        ColorRGBA               m_Color         = {}; //ImColor
    };

    struct input_pin : pin
    {
        connection_guid         m_ConnectionGUID = {};
        XPROPERTY_DEF
        ( "input_pin", input_pin
        , obj_base<pin>
        , obj_member<"ConnectionGUID", &input_pin::m_ConnectionGUID, member_flags<flags::DONT_SHOW> >
        )
    };
    XPROPERTY_REG(input_pin)

    struct output_pin : pin
    {
        std::vector<pin>	m_SubElements = {};

        XPROPERTY_DEF
        ( "output_pin", output_pin
        , obj_base<pin>
        , obj_member<"subelements", &output_pin::m_SubElements, member_flags<flags::DONT_SHOW, flags::DONT_SAVE> >    // backwards compatibility
        , obj_member<"SubElements", &output_pin::m_SubElements, member_flags<flags::DONT_SHOW> >
        )
    };
    XPROPERTY_REG(output_pin)

} // end namespace xmaterial_compiler

#endif

