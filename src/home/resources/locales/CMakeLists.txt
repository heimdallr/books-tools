cmake_minimum_required (VERSION 3.20)

set(EXECUTABLE_EXT)
if (WIN32)
	set(EXECUTABLE_EXT ".exe")
endif()

set(QT_LUPDATE_TOOL ${QT_BIN_DIR}/lupdate${EXECUTABLE_EXT})
if (NOT EXISTS ${QT_LUPDATE_TOOL})
	message(FATAL_ERROR "QT_LUPDATE_TOOL not found: ${QT_LUPDATE_TOOL}")
endif()
set(QT_LRELEASE_TOOL ${QT_BIN_DIR}/lrelease${EXECUTABLE_EXT})
if (NOT EXISTS ${QT_LRELEASE_TOOL})
	message(FATAL_ERROR "QT_LRELEASE_TOOL not found: ${QT_LRELEASE_TOOL}")
endif()

set(locales en ru uk)

set(LRELEASE_LIST)
set(LUPDATE_LIST)

foreach(locale ${locales})
    set(QMFILE "${CMAKE_BINARY_DIR}/bin/locales/flitools_${locale}.qm")
    set(TSFILE "${CMAKE_CURRENT_LIST_DIR}/${locale}.ts")
    
	install(FILES ${QMFILE} DESTINATION "locales/")
    list(APPEND LRELEASE_LIST "${TSFILE} -qm ${QMFILE}")
    list(APPEND LUPDATE_LIST -ts "${TSFILE}")
endforeach()

set(LUPDATE_COMMAND_PARAMETERS
	"${FOUNDATION_ROOT}/util/GenresLocalization.h"
	-locations none
	-extensions cpp,h,ui
	-sort-messages
	-no-obsolete
)

set(LUPDATE_COMMAND
	"${QT_LUPDATE_TOOL}"
	${LUPDATE_COMMAND_PARAMETERS}
	${LUPDATE_LIST}
)

execute_process(
    COMMAND ${LUPDATE_COMMAND}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../../
)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/locales")
foreach(LRELEASE_PARAMETERS ${LRELEASE_LIST})
	separate_arguments(LRELEASE_PARAMETERS)
	execute_process(
    	COMMAND ${QT_LRELEASE_TOOL} ${LRELEASE_PARAMETERS}
	    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../../
	)
endforeach()
