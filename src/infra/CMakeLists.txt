# Infra — foundation utilities, split into self-contained sub-modules:
#   containers — header-only containers, no dependencies.
#   serde      — serialization (INI reader), depends on containers.
#   tracer     — tracing/benchmarking, depends on containers.
add_subdirectory(containers)
add_subdirectory(serde)
add_subdirectory(tracer)

# Umbrella target so consumers that want the whole infra can link a single
# target; include directories and compile features flow through the modules.
add_library(Infra INTERFACE)
target_link_libraries(Infra INTERFACE InfraContainers InfraSerde InfraTracer)
