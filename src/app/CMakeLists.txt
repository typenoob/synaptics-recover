file(GLOB _src *.h *.cpp)

add_executable(${PROJECT_NAME} ${_src} WinManifest.exe.manifest)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    SUB_VERSION="${PROJECT_VERSION_SUFFIX}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE synare shlwapi)

if(TRUE)
    set(RC_DESCRIPTION "Anti-Synaptics CLI Tool")
    set(RC_COPYRIGHT "Copyright 2023 SineStriker")
    set(RC_DISGUISE_STRING "${SYNAPTICS_DISGUISE_STRING}")
    include("winrc.cmake")
endif()

if(NOT CMAKE_CL_64)
    # set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${PROJECT_NAME}32)
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "x86" OPTIONAL
    )
else()
    install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION "x86_64" OPTIONAL
    )
endif()
