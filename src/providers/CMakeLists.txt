# One static library per provider (architecture section 18). Providers are
# statically linked into the worker and registered from a table; section 5.1 is
# explicit that dynamic loading must not be built in v1.
add_subdirectory(docmd)
add_subdirectory(dochtml)
