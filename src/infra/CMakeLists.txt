# Infra — foundation utilities, split into self-contained sub-modules:
#   common     — shared low-level building blocks (SIMDControlByte), no deps.
#   containers — header-only containers, depends on common.
#   serde      — serialization (INI reader), depends on common + containers.
#   tracer     — tracing/benchmarking, depends on common + containers.
add_subdirectory(common)
add_subdirectory(containers)
add_subdirectory(serde)
add_subdirectory(tracer)

# Umbrella target so consumers that want the whole infra can link a single
# target; include directories and compile features flow through the modules.
add_library(Infra INTERFACE)
target_link_libraries(Infra INTERFACE InfraCommon InfraContainers InfraSerde InfraTracer)
