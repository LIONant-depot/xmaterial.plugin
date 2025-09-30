#ifndef XMATERIAL_RSC_DESCRIPTOR_H
#define XMATERIAL_RSC_DESCRIPTOR_H

namespace xmaterial_rsc
{
    // While this should be just a type... it also happens to be an instance... the instance of the texture_plugin
    // So while generating the type guid we must treat it as an instance.
    inline static constexpr auto resource_type_guid_v = xresource::type_guid(xresource::guid_generator::Instance64FromString("Material"));

    struct descriptor final : xresource_pipeline::descriptor::base
    {
        bool m_bAlpha;
        XPROPERTY_VDEF("Material", descriptor
            , obj_member< "bAlpha", &descriptor::m_bAlpha
            , member_help<"Specifies how the texture will be used. For example, "
            "it can be used for regular color images, images with transparency, "
            "high dynamic range (HDR) images, normal maps (used for adding detail "
            "to 3D models), or intensity maps (like masks). It's like telling the "
            "system what kind of picture you're working with."
            >>
        )
    };
    XPROPERTY_VREG(descriptor)

        //--------------------------------------------------------------------------------------

        struct factory final : xresource_pipeline::factory_base
    {
        using xresource_pipeline::factory_base::factory_base;

        std::unique_ptr<xresource_pipeline::descriptor::base> CreateDescriptor(void) const noexcept override
        {
            return std::make_unique<descriptor>();
        };

        xresource::type_guid ResourceTypeGUID(void) const noexcept override
        {
            return resource_type_guid_v;
        }

        const char* ResourceTypeName(void) const noexcept override
        {
            return "Material";
        }

        const xproperty::type::object& ResourceXPropertyObject(void) const noexcept override
        {
            return *xproperty::getObjectByType<descriptor>();
        }
    };

    inline static factory g_Factory{};
};

#endif
