# Single source of truth for the version. Everything else derives from it:
# project(), the VERSIONINFO resource, and the container's META record.
set(LWI_VERSION_MAJOR 0)
set(LWI_VERSION_MINOR 3)
set(LWI_VERSION_PATCH 0)

set(LWI_VERSION "${LWI_VERSION_MAJOR}.${LWI_VERSION_MINOR}.${LWI_VERSION_PATCH}")
set(LWI_VERSION_RC "${LWI_VERSION_MAJOR},${LWI_VERSION_MINOR},${LWI_VERSION_PATCH},0")
set(LWI_VERSION_RC_STR "${LWI_VERSION}.0")

set(LWI_COMPANY   "Locke Werks")
set(LWI_COPYRIGHT "Copyright (c) 2026 Locke Werks")
