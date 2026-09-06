target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/apex_packet.c)
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/apex_connection.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
