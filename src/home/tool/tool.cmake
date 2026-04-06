if (MSVC)
	#Да, колхоз. Но я устал
	CopyAndInstallICU(tu dt uc in)
	CopyAndInstallQt(${QtModules})
	InstallQtPlugins(imageformats platforms styles tls)
endif()

file(GLOB sources_list LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/*")
foreach(dir ${sources_list})
	if(IS_DIRECTORY ${dir})
		get_filename_component(module ${dir} NAME)
		include("${dir}/${module}.cmake")
	endif()
endforeach()

set(COMPANY_NAME "HomeCompa")
configure_file(${BUILDSCRIPTS_ROOT}/helpers/git_hash.h.in ${CMAKE_CURRENT_BINARY_DIR}/config/git_hash.h @ONLY)
configure_file(${BUILDSCRIPTS_ROOT}/helpers/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/config/version.h @ONLY)
