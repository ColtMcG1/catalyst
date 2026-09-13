#include "../detail_backend.hpp"

namespace catalyst::rendering::detail
{

    const char *backend_name()
    {
        return "d3d12";
    }

    backend_kind backend_type() noexcept
    {
        return backend_kind::d3d12;
    }

} // namespace catalyst::rendering::detail
