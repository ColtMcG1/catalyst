#include "../detail_backend.hpp"

namespace catalyst::rendering::detail
{

    const char *backend_name()
    {
        return "null";
    }

    backend_kind backend_type() noexcept
    {
        return backend_kind::null;
    }

} // namespace catalyst::rendering::detail
