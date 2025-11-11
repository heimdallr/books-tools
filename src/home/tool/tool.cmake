#Да, колхоз. Но я устал
set(D)
if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
	set(D d)
endif()

set(QT_BIN_FILES
	${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}/Qt6Core${D}.dll
	${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}/Qt6Gui${D}.dll
	${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}/Qt6Network${D}.dll
)
file(COPY ${QT_BIN_FILES} DESTINATION ${CMAKE_BINARY_DIR}/bin)

file(GLOB sources_list LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/*")
foreach(dir ${sources_list})
	if(IS_DIRECTORY ${dir})
		get_filename_component(module ${dir} NAME)
		include("${dir}/${module}.cmake")
	endif()
endforeach()

configure_file(${BUILDSCRIPTS_ROOT}/helpers/git_hash.h.in ${CMAKE_CURRENT_BINARY_DIR}/config/git_hash.h @ONLY)
configure_file(${BUILDSCRIPTS_ROOT}/helpers/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/config/version.h @ONLY)
