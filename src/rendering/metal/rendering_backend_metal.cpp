#include "../detail_backend.hpp"

namespace catalyst::rendering::detail
{

    const char *backend_name()
    {
        return "metal";
    }

    backend_kind backend_type() noexcept
    {
        return backend_kind::metal;
    }

} // namespace catalyst::rendering::detail
