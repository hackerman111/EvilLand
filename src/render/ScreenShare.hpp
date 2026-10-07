#pragma once

#include "../helpers/memory/Memory.hpp"

class CWLSurfaceResource;

namespace Render {
    bool surfaceHiddenFromScreenShare(SP<CWLSurfaceResource> surface);
}
