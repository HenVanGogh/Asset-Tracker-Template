if(CONFIG_APP_SELFTEST)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/selftest.c)
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.)
endif()
