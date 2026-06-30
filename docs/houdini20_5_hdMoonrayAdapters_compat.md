# Superseded hdMoonray Adapter Compatibility Note

This historical note used to describe a temporary local header workaround for an older Houdini/USD/macOS toolchain combination.

That workaround is no longer part of the Houdini 21 platform-alignment patch. The canonical Houdini 21 build should compile against the active Houdini 21 toolkit headers directly, without private compatibility include paths or target-local shim headers.

Retain this file only as a marker that the old workaround was intentionally removed from active build logic.
