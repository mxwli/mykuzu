add_library(kuzu_storage_index
        OBJECT
        base_hash_index.cpp
        hash_index.cpp
        hash_index_builder.cpp)

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:kuzu_storage_index>
        PARENT_SCOPE)
