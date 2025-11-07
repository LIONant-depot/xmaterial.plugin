#include "xmaterial_graph_node.h"

namespace xmaterial_graph
{
    int node::getInputPinIndex(pin_guid Guid)
    {
        for (int i = 0, end = static_cast<int>(m_InputPins.size()); i < end; ++i)
        {
            if (m_InputPins[i].m_PinGUID == Guid) return i;
        }
        return -1;
    }
}
