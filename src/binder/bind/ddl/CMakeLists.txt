add_library(kuzu_binder_bind_ddl
        OBJECT
        bind_create_rdf_graph.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:kuzu_binder_bind_ddl>
        PARENT_SCOPE)
